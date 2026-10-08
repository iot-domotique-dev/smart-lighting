import { useCallback, useEffect, useRef, useState } from 'react';
import { ActivityIndicator, Alert, AppState, SafeAreaView, StyleSheet, Text, View } from 'react-native';
import { StatusBar } from 'expo-status-bar';
import { CoreApiError, describeApiError, describeCommandFailure, getCommand,
  getDevice, lampsOnly, submitPower } from './src/api/coreApiClient';
import type { LampRecord, PowerState, V7Id } from './src/api/types';
import { ConnectionScreen } from './src/features/core/ConnectionScreen';
import { LampListScreen } from './src/features/lighting/LampListScreen';
import { acquireLampCommand, isCommandInProgress, pollCommand, releaseLampCommand } from './src/features/lighting/commandPolling';
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

export default function App() {
  const [settings, setSettings] = useState<ConnectionSettings | null>(null);
  const [loading, setLoading] = useState(true);
  const [editing, setEditing] = useState(false);
  const [commandFeedback, setCommandFeedback] = useState<Record<string, LampCommandFeedback>>({});
  const commandLocks = useRef(new Set<V7Id>());
  const commandRuns = useRef(new Map<V7Id, AbortController>());
  const mounted = useRef(true);

  function updateCommand(lampId: V7Id, feedback: LampCommandFeedback) {
    if (!mounted.current) return;
    setCommandFeedback((current) => ({ ...current, [lampId]: feedback }));
  }

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      commandRuns.current.forEach((controller) => controller.abort());
      commandRuns.current.clear();
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

  const sendPower = useCallback(async (lampId: V7Id, requested: PowerState)
    : Promise<LampRecord | null> => {
    const connection = settings;
    if (!connection || !acquireLampCommand(commandLocks.current, lampId)) return null;
    const controller = new AbortController();
    commandRuns.current.set(lampId, controller);
    updateCommand(lampId, { phase: 'submitting', requested });
    let commandId: V7Id | undefined;
    let result: LampCommandFeedback = { phase: 'rejected', requested,
      message: 'La commande n’a pas pu démarrer.' };
    let updatedLamp: LampRecord | null = null;

    try {
      try {
        const submitted = await submitPower(connection.ip, lampId, requested,
          connection.token, controller.signal);
        commandId = submitted.command.id;
        updateCommand(lampId, { phase: 'tracking', requested, commandId,
          commandState: 'sent', retries: 0 });

        const outcome = await pollCommand(commandId,
          () => getCommand(connection.ip, commandId!, connection.token, controller.signal), {
            signal: controller.signal,
            waitUntilActive: waitUntilAppActive,
            onStatus: (command) => {
              if (command.state === 'sent' || command.state === 'accepted') {
                updateCommand(lampId, { phase: 'tracking', requested, commandId: command.id,
                  commandState: command.state, retries: command.retries });
              }
            },
          });

        if (outcome.kind === 'terminal') {
          const command = outcome.command;
          if (command.state === 'executed') {
            result = { phase: 'executed', requested, commandId, retries: command.retries };
          } else if (command.state === 'failed') {
            result = { phase: 'failed', requested, commandId, retries: command.retries,
              message: describeCommandFailure(command.error) };
          } else {
            result = { phase: 'uncertain', requested, commandId, retries: command.retries,
              message: 'Délai CORE expiré : l’exécution réelle reste inconnue.' };
          }
        } else if (outcome.kind === 'not_found') {
          result = { phase: 'uncertain', requested, commandId,
            message: 'Suivi introuvable (redémarrage ou éviction possible). Résultat incertain.' };
        } else if (outcome.kind === 'unavailable') {
          result = { phase: 'uncertain', requested, commandId,
            message: `Suivi interrompu : ${describeApiError(outcome.error)} Résultat incertain.` };
        } else if (outcome.kind === 'timeout') {
          result = { phase: 'uncertain', requested, commandId,
            message: 'Délai de suivi mobile dépassé. L’exécution est inconnue; actualisez l’état.' };
        } else {
          result = { phase: 'uncertain', requested, commandId,
            message: 'Suivi interrompu. Le résultat de la commande est incertain.' };
        }
      } catch (error) {
        if (error instanceof CoreApiError && error.kind === 'submission_uncertain') {
          result = { phase: 'uncertain', requested, message: error.message };
        } else {
          result = { phase: 'rejected', requested, message: describeApiError(error) };
        }
      }

      if (mounted.current && !controller.signal.aborted) {
        await waitUntilAppActive(controller.signal);
      }
      if (mounted.current && !controller.signal.aborted) {
        updateCommand(lampId, { phase: 'refreshing', requested,
          message: 'Relecture de l’état actuel depuis le CORE…' });
        try {
          const response = await getDevice(connection.ip, lampId);
          updatedLamp = lampsOnly([response.device])[0] ?? null;
        } catch (error) {
          result = { ...result, refreshError: describeApiError(error) };
        }
        updateCommand(lampId, result);
      }
    } finally {
      if (commandRuns.current.get(lampId) === controller) commandRuns.current.delete(lampId);
      releaseLampCommand(commandLocks.current, lampId);
    }
    return updatedLamp;
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
        <LampListScreen ip={settings.ip} onSettings={requestSettingsEdit}
          commandFeedback={commandFeedback} onPower={sendPower} /> :
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
