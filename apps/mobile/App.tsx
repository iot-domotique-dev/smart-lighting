import { useCallback, useEffect, useRef, useState } from 'react';
import { ActivityIndicator, Alert, AppState, SafeAreaView, StyleSheet, Text, View } from 'react-native';
import { StatusBar } from 'expo-status-bar';
import { CoreApiError, describeApiError, describeCommandFailure, getCommand,
  getDevice, lampsOnly, submitBrightness, submitPower } from './src/api/coreApiClient';
import type { LampRecord, PowerState, V7Id } from './src/api/types';
import { ConnectionScreen } from './src/features/core/ConnectionScreen';
import { LampListScreen } from './src/features/lighting/LampListScreen';
import { acquireLampCommand, canInterruptWithPriorityOff, isCommandInProgress,
  isCurrentLampCommand,
  pollCommand, releaseLampCommand } from './src/features/lighting/commandPolling';
import type { LampCommandFeedback } from './src/features/lighting/commandPolling';
import { clearConnection, loadConnection, saveConnection } from './src/settings/connectionSettings';
import type { ConnectionSettings } from './src/settings/connectionSettings';

function waitUntilAppActive(signal?: AbortSignal): Promise<void> {
  const state = AppState.currentState;
  if (!signal || signal.aborted || (state !== 'background' && state !== 'inactive')) {
    return Promise.resolve();
  }
  return new Promise((resolve) => {
    let subscription: { remove: () => void } | undefined;
    const finish = () => {
      subscription?.remove();
      signal.removeEventListener('abort', finish);
      resolve();
    };
    subscription = AppState.addEventListener('change', (state) => {
      if (state === 'active') finish();
    });
    signal.addEventListener('abort', finish, { once: true });
    const currentState = AppState.currentState;
    if (currentState !== 'background' && currentState !== 'inactive') finish();
  });
}

type LampOutputRequest =
  | { kind: 'power'; state: PowerState }
  | { kind: 'brightness'; percent: number };

function requestedPower(request: LampOutputRequest): PowerState {
  return request.kind === 'power' ? request.state : request.percent === 0 ? 'off' : 'on';
}

function requestBrightness(request: LampOutputRequest): number | undefined {
  return request.kind === 'brightness' ? request.percent : undefined;
}

export default function App() {
  const [settings, setSettings] = useState<ConnectionSettings | null>(null);
  const [loading, setLoading] = useState(true);
  const [editing, setEditing] = useState(false);
  const [commandFeedback, setCommandFeedback] = useState<Record<string, LampCommandFeedback>>({});
  const commandFeedbackRef = useRef<Record<string, LampCommandFeedback>>({});
  const commandLocks = useRef(new Set<V7Id>());
  const commandRuns = useRef(new Map<V7Id, Set<AbortController>>());
  const commandOwners = useRef(new Map<V7Id, AbortController>());
  const mounted = useRef(true);

  function updateCommand(lampId: V7Id, feedback: LampCommandFeedback,
    owner?: AbortController) {
    if (!mounted.current) return;
    if (owner && commandOwners.current.get(lampId) !== owner) return;
    const next = { ...commandFeedbackRef.current, [lampId]: feedback };
    commandFeedbackRef.current = next;
    setCommandFeedback(next);
  }

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      commandRuns.current.forEach((controllers) => {
        controllers.forEach((controller) => controller.abort());
      });
      commandRuns.current.clear();
      commandOwners.current.clear();
    };
  }, []);

  useEffect(() => {
    let active = true;
    loadConnection().then((saved) => {
      if (active) setSettings(saved);
    }).catch(() => {
      if (active) setSettings(null);
    }).finally(() => {
      if (active) setLoading(false);
    });
    return () => { active = false; };
  }, []);

  const sendOutput = useCallback(async (lampId: V7Id, request: LampOutputRequest)
    : Promise<LampRecord | null> => {
    const connection = settings;
    const priorFeedback = commandFeedbackRef.current[String(lampId)];
    const priorOwner = commandOwners.current.get(lampId);
    const priorityOff = request.kind === 'power' && request.state === 'off' &&
      commandLocks.current.has(lampId) && canInterruptWithPriorityOff(priorFeedback);
    if (!connection || (!priorityOff &&
        !acquireLampCommand(commandLocks.current, lampId))) return null;
    const requested = requestedPower(request);
    const brightnessPercent = requestBrightness(request);
    const requestFields = brightnessPercent === undefined ? {} : { brightnessPercent };
    const controller = new AbortController();
    const runs = commandRuns.current.get(lampId) ?? new Set<AbortController>();
    runs.add(controller);
    commandRuns.current.set(lampId, runs);
    commandOwners.current.set(lampId, controller);
    updateCommand(lampId, { phase: 'submitting', requested, ...requestFields }, controller);
    let commandId: V7Id | undefined;
    let supersededByNewerOutput = false;
    let result: LampCommandFeedback = { phase: 'rejected', requested,
      ...requestFields, message: 'La commande n’a pas pu démarrer.' };
    let updatedLamp: LampRecord | null = null;

    try {
      try {
        const submitted = request.kind === 'power'
          ? await submitPower(connection.ip, lampId, request.state, connection.token,
            controller.signal)
          : await submitBrightness(connection.ip, lampId, request.percent, connection.token,
            controller.signal);
        commandId = submitted.command.id;
        updateCommand(lampId, { phase: 'tracking', requested, commandId,
          commandState: 'sent', retries: 0, ...requestFields }, controller);
        if (priorityOff) {
          for (const previous of runs) {
            if (previous !== controller) previous.abort();
          }
        }

        const outcome = await pollCommand(commandId,
          () => getCommand(connection.ip, commandId!, connection.token, controller.signal), {
            signal: controller.signal,
            waitUntilActive: waitUntilAppActive,
            onStatus: (command) => {
              if (command.state === 'sent' || command.state === 'accepted') {
                updateCommand(lampId, { phase: 'tracking', requested, commandId: command.id,
                  commandState: command.state, retries: command.retries, ...requestFields },
                  controller);
              }
            },
          });

        if (outcome.kind === 'terminal') {
          const command = outcome.command;
          if (command.state === 'executed') {
            result = { phase: 'executed', requested, commandId, retries: command.retries,
              ...requestFields };
          } else if (command.state === 'failed') {
            result = { phase: 'failed', requested, commandId, retries: command.retries,
              message: describeCommandFailure(command.error), ...requestFields };
          } else {
            result = { phase: 'uncertain', requested, commandId, retries: command.retries,
              message: 'Délai CORE expiré : l’exécution réelle reste inconnue.', ...requestFields };
          }
        } else if (outcome.kind === 'not_found') {
          result = { phase: 'uncertain', requested, commandId,
            message: 'Suivi introuvable (redémarrage ou éviction possible). Résultat incertain.',
            ...requestFields };
        } else if (outcome.kind === 'unavailable') {
          result = { phase: 'uncertain', requested, commandId,
            message: `Suivi interrompu : ${describeApiError(outcome.error)} Résultat incertain.`,
            ...requestFields };
        } else if (outcome.kind === 'timeout') {
          result = { phase: 'uncertain', requested, commandId,
            message: 'Délai de suivi mobile dépassé. L’exécution est inconnue; actualisez l’état.',
            ...requestFields };
        } else {
          result = { phase: 'uncertain', requested, commandId,
            message: 'Suivi interrompu. Le résultat de la commande est incertain.', ...requestFields };
        }
      } catch (error) {
        if (error instanceof CoreApiError && error.kind === 'submission_uncertain') {
          result = { phase: 'uncertain', requested, message: error.message, ...requestFields };
        } else {
          result = { phase: 'rejected', requested, message: describeApiError(error),
            ...requestFields };
        }
      }

      if (priorityOff && priorFeedback?.phase === 'tracking' &&
          result.phase === 'rejected' && priorOwner &&
          !priorOwner.signal.aborted) {
        commandOwners.current.set(lampId, priorOwner);
        updateCommand(lampId, { ...priorFeedback,
          priorityOffError: result.message }, priorOwner);
        return null;
      }

      if (mounted.current && !controller.signal.aborted) {
        await waitUntilAppActive(controller.signal);
      }
      if (mounted.current && !controller.signal.aborted) {
        updateCommand(lampId, { phase: 'refreshing', requested, ...requestFields,
          message: 'Relecture de l’état actuel depuis le CORE…' }, controller);
        try {
          const response = await getDevice(connection.ip, lampId);
          updatedLamp = lampsOnly([response.device])[0] ?? null;
          if (!updatedLamp) throw new Error('La réponse ne contient plus cette lampe.');
          if (result.phase === 'uncertain') {
            const priorMessage = result.message;
            const retries = result.retries;
            result = { phase: 'reconciled', requested, commandId,
              ...(retries === undefined ? {} : { retries }), ...requestFields,
              message: `${priorMessage} État relu depuis le CORE; le résultat de la commande reste incertain. Aucun nouveau POST n’a été envoyé.` };
          }
        } catch (error) {
          result = { ...result, refreshError: describeApiError(error) };
        }
        updateCommand(lampId, result, controller);
      }
    } finally {
      const activeRuns = commandRuns.current.get(lampId);
      activeRuns?.delete(controller);
      if (activeRuns?.size === 0) commandRuns.current.delete(lampId);
      const stillOwner = isCurrentLampCommand(commandOwners.current,
        lampId, controller);
      supersededByNewerOutput = !stillOwner;
      if (stillOwner &&
          result.phase !== 'uncertain') {
        commandOwners.current.delete(lampId);
        releaseLampCommand(commandLocks.current, lampId);
      }
    }
    // A GET already in flight for an older output command must never replace
    // the lamp view after a newer priority OFF has taken ownership.
    return supersededByNewerOutput ? null : updatedLamp;
  }, [settings]);

  const sendPower = useCallback((lampId: V7Id, state: PowerState) =>
    sendOutput(lampId, { kind: 'power', state }), [sendOutput]);

  const sendBrightness = useCallback((lampId: V7Id, percent: number) =>
    sendOutput(lampId, { kind: 'brightness', percent }), [sendOutput]);

  const reconcileLamp = useCallback(async (lampId: V7Id): Promise<LampRecord | null> => {
    const connection = settings;
    const feedback = commandFeedbackRef.current[String(lampId)];
    if (!connection || feedback?.phase !== 'uncertain' ||
        !commandLocks.current.has(lampId)) return null;
    updateCommand(lampId, { phase: 'refreshing', requested: feedback.requested,
      ...(feedback.brightnessPercent === undefined ? {} :
        { brightnessPercent: feedback.brightnessPercent }),
      message: 'Nouvelle lecture de l’état avant de réactiver les commandes…' });
    try {
      const response = await getDevice(connection.ip, lampId);
      const lamp = lampsOnly([response.device])[0] ?? null;
      if (!lamp) throw new Error('La réponse ne contient plus cette lampe.');
      updateCommand(lampId, { phase: 'reconciled', requested: feedback.requested,
        ...(feedback.commandId === undefined ? {} : { commandId: feedback.commandId }),
        ...(feedback.retries === undefined ? {} : { retries: feedback.retries }),
        ...(feedback.brightnessPercent === undefined ? {} :
          { brightnessPercent: feedback.brightnessPercent }),
        message: `${feedback.message} État relu. Le résultat précédent reste incertain; vérifiez la consigne confirmée. Aucun POST n’a été répété.` });
      releaseLampCommand(commandLocks.current, lampId);
      commandOwners.current.delete(lampId);
      return lamp;
    } catch (error) {
      updateCommand(lampId, { ...feedback, refreshError: describeApiError(error) });
      return null;
    }
  }, [settings]);

  function requestSettingsEdit() {
    if (commandLocks.current.size > 0) {
      Alert.alert('Commande en cours',
        'Attendez la fin du suivi avant de modifier l’adresse ou le jeton du CORE.');
      return;
    }
    setEditing(true);
  }

  async function save(ip: string, tokenInput: string) {
    const token = tokenInput.trim() || settings?.token || '';
    const next = { ip, token };
    await saveConnection(next);
    setSettings(next);
    setEditing(false);
  }

  async function clear() {
    await clearConnection();
    setSettings(null);
    setEditing(false);
  }

  return (
    <SafeAreaView style={styles.root}>
      <StatusBar style="dark" />
      {loading ? <View style={styles.loading}>
        <ActivityIndicator size="large" color="#18785d" />
        <Text style={styles.loadingText}>Ouverture de Smart Lighting…</Text>
      </View> : settings && !editing ?
        <LampListScreen ip={settings.ip} token={settings.token} onSettings={requestSettingsEdit}
          commandFeedback={commandFeedback} onPower={sendPower} onBrightness={sendBrightness}
          onReconcile={reconcileLamp} /> :
        <ConnectionScreen savedIp={settings?.ip} hasSavedToken={Boolean(settings?.token)}
          onSave={save} onCancel={settings ? () => setEditing(false) : undefined}
          onDelete={settings ? clear : undefined} />}
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#f4f7f4' },
  loading: { flex: 1, justifyContent: 'center', alignItems: 'center' },
  loadingText: { color: '#637480', fontSize: 14, marginTop: 16 },
});
