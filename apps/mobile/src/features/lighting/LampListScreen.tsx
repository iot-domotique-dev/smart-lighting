import { useCallback, useEffect, useRef, useState } from 'react';
import * as Crypto from 'expo-crypto';
import { ActivityIndicator, Alert, AppState, Modal, Pressable, RefreshControl,
  ScrollView, StyleSheet, Text, View } from 'react-native';
import { CoreApiError, describeApiError, getDevice, getDevices, getHealth, getModules,
  getRemovalOperation, lampsOnly, requestDeviceRemoval } from '../../api/coreApiClient';
import type { HealthResponse, LampRecord, ModuleRecord, PowerState,
  V7Id } from '../../api/types';
import { StatusIndicator } from '../../components/StatusIndicator';
import { LampCard } from './LampCard';
import type { LampCommandFeedback } from './commandPolling';
import { PairingScreen } from '../pairing/PairingScreen';
import { acquireRemovalAction, canRequestRemoval, createRemovalConfirmation,
  createRemovalOperationId, isCommandBlockingRemoval,
  isRemovalBlockingCommand, isRemovalInProgress, pollRemovalOperation,
  releaseRemovalAction } from './removalWorkflow';
import type { RemovalPollResult } from './removalWorkflow';
import { clearPendingRemoval, loadPendingRemoval, savePendingRemoval } from './removalOperationStore';
import type { PendingRemovalOperation, StoredRemovalPhase } from './removalOperationStore';

const POLL_INTERVAL_MS = 20000;

function waitUntilRemovalAppActive(signal?: AbortSignal): Promise<void> {
  if (!signal || signal.aborted || AppState.currentState === 'active') return Promise.resolve();
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
    if (AppState.currentState === 'active') finish();
  });
}

interface Snapshot {
  health: HealthResponse;
  modules: ModuleRecord[];
  lamps: LampRecord[];
  receivedAt: number;
}

export function LampListScreen({ ip, token, onSettings, commandFeedback, onPower }: {
  ip: string;
  token: string;
  onSettings: () => void;
  commandFeedback: Record<string, LampCommandFeedback>;
  onPower: (lampId: V7Id, state: PowerState) => Promise<LampRecord | null>;
}) {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [coreReachable, setCoreReachable] = useState<boolean | null>(null);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [revalidating, setRevalidating] = useState(false);
  const [selectedId, setSelectedId] = useState<V7Id | null>(null);
  const [detail, setDetail] = useState<LampRecord | null>(null);
  const [detailError, setDetailError] = useState('');
  const [detailLoading, setDetailLoading] = useState(false);
  const [pairingOpen, setPairingOpen] = useState(false);
  const [removal, setRemoval] = useState<PendingRemovalOperation | null>(null);
  const [removalModalOpen, setRemovalModalOpen] = useState(false);
  const [removalStarting, setRemovalStarting] = useState(false);
  const [removalLoading, setRemovalLoading] = useState(false);
  const [removalMessage, setRemovalMessage] = useState('');
  const [removalStorageError, setRemovalStorageError] = useState('');
  const requestVersion = useRef(0);
  const detailVersion = useRef(0);
  const screenMounted = useRef(true);
  const removalRef = useRef<PendingRemovalOperation | null>(null);
  const removalStartLock = useRef({ active: false });
  const removalTracker = useRef<AbortController | null>(null);
  const old = Boolean(error) || revalidating;

  const refresh = useCallback(async (manual = false) => {
    const version = ++requestVersion.current;
    if (manual) setRefreshing(true);
    try {
      const [health, modules, devices] = await Promise.allSettled([
        getHealth(ip), getModules(ip), getDevices(ip),
      ]);
      if (version !== requestVersion.current) return;
      setCoreReachable([health, modules, devices].some((result) => result.status === 'fulfilled'));
      if (health.status === 'fulfilled' && modules.status === 'fulfilled' &&
          devices.status === 'fulfilled') {
        setSnapshot({ health: health.value, modules: modules.value.modules,
          lamps: lampsOnly(devices.value.devices), receivedAt: Date.now() });
        setError('');
      } else {
        const failure = [health, modules, devices].find(
          (result): result is PromiseRejectedResult => result.status === 'rejected',
        );
        setError(describeApiError(failure?.reason));
      }
      setRevalidating(false);
    } catch (failure) {
      if (version !== requestVersion.current) return;
      setCoreReachable(false);
      setError(describeApiError(failure));
      setRevalidating(false);
    } finally {
      if (version === requestVersion.current) {
        setLoading(false);
        setRefreshing(false);
      }
    }
  }, [ip]);

  const applyRemoval = useCallback((operation: PendingRemovalOperation | null) => {
    removalRef.current = operation;
    if (screenMounted.current) setRemoval(operation);
  }, []);

  const persistRemoval = useCallback(async (operation: PendingRemovalOperation) => {
    await savePendingRemoval(operation);
    applyRemoval(operation);
  }, [applyRemoval]);

  const refreshRemovalInventory = useCallback(async (deviceId: V7Id)
    : Promise<'present' | 'missing' | 'unavailable'> => {
    try {
      const response = await getDevices(ip);
      const lamps = lampsOnly(response.devices);
      if (screenMounted.current) {
        setSnapshot((current) => current ? { ...current, lamps, receivedAt: Date.now() } : current);
      }
      return lamps.some((lamp) => lamp.id === deviceId) ? 'present' : 'missing';
    } catch {
      return 'unavailable';
    }
  }, [ip]);

  const finishRemovalSuccess = useCallback(async (operation: PendingRemovalOperation) => {
    const succeeded = { ...operation, phase: 'succeeded' as const };
    try {
      await persistRemoval(succeeded);
    } catch {
      applyRemoval(succeeded);
      setRemovalStorageError('Le résultat est confirmé, mais sa reprise locale n’a pas pu être sauvegardée. Gardez l’application ouverte et actualisez l’inventaire.');
    }
    const inventory = await refreshRemovalInventory(operation.deviceId);
    if (inventory === 'missing') {
      try {
        await clearPendingRemoval();
        applyRemoval(null);
        setRemovalStorageError('');
        setRemovalModalOpen(false);
        setSelectedId(null);
        Alert.alert('Lampe retirée', `${operation.deviceName} a été retirée de Smart Lighting. Elle pourra être ajoutée à nouveau depuis le parcours d’appairage.`);
      } catch {
        applyRemoval(succeeded);
        setRemovalMessage('Le retrait est confirmé et la lampe a disparu de l’inventaire, mais l’état local sécurisé n’a pas pu être effacé. Actualisez à nouveau avant tout autre retrait.');
      }
    } else if (inventory === 'present') {
      setRemovalMessage('Le CORE confirme le retrait, mais la lampe apparaît encore dans l’inventaire. Actualisez l’inventaire; ne recommencez pas le retrait.');
    } else {
      setRemovalMessage('Le CORE confirme le retrait, mais l’inventaire n’a pas pu être relu. Réessayez uniquement l’actualisation de l’inventaire.');
    }
  }, [applyRemoval, persistRemoval, refreshRemovalInventory]);

  const finishRemovalFailure = useCallback(async (operation: PendingRemovalOperation,
    resultCode?: string) => {
    const failed = { ...operation, phase: 'failed' as const };
    try {
      await clearPendingRemoval();
      applyRemoval(null);
      setRemovalStorageError('');
      setRemovalModalOpen(false);
      Alert.alert('Retrait échoué', resultCode ?
        `Le CORE a signalé : ${resultCode}. La lampe reste dans Smart Lighting.` :
        'Le CORE a signalé un échec. La lampe reste dans Smart Lighting.');
    } catch {
      applyRemoval(failed);
      setRemovalMessage('Le CORE a confirmé l’échec, mais l’état local n’a pas pu être effacé. La lampe n’a pas été retirée de l’application.');
      setRemovalStorageError('Le résultat terminal du retrait ne peut pas être effacé de SecureStore.');
    }
  }, [applyRemoval]);

  const handleRemovalPoll = useCallback(async (result: RemovalPollResult,
    operation: PendingRemovalOperation) => {
    if (result.kind === 'cancelled') return;
    if (result.kind === 'succeeded') {
      await finishRemovalSuccess(operation);
      return;
    }
    if (result.kind === 'failed') {
      await finishRemovalFailure(operation, result.operation.result_code);
      return;
    }
    if (result.kind === 'not_found') {
      const updated = { ...operation, phase: 'not_found' as const };
      try { await persistRemoval(updated); } catch {
        applyRemoval(updated);
        setRemovalStorageError('Le suivi doit rester enregistré pour éviter de renvoyer le POST.');
      }
      setRemovalMessage('Le CORE ne retrouve pas cette opération. Cela ne prouve pas que le POST n’a jamais été reçu. Aucun nouveau POST ne sera envoyé; vous pouvez relire le même identifiant.');
      return;
    }
    const uncertain = { ...operation, phase: 'uncertain' as const };
    try { await persistRemoval(uncertain); } catch {
      applyRemoval(uncertain);
      setRemovalStorageError('Le suivi doit rester enregistré pour éviter de renvoyer le POST.');
    }
    if (result.kind === 'unavailable') {
      setRemovalMessage(`Suivi interrompu : ${describeApiError(result.error)} Le résultat reste incertain; reprenez le suivi avec le même identifiant.`);
    } else {
      setRemovalMessage('Le CORE n’a pas encore fourni de résultat final. Le retrait reste incertain; reprenez le suivi avec le même identifiant.');
    }
  }, [applyRemoval, finishRemovalFailure, finishRemovalSuccess, persistRemoval]);

  const followRemoval = useCallback(async (operation: PendingRemovalOperation) => {
    if (removalTracker.current) return;
    if (operation.coreIp !== ip) {
      setRemovalMessage(`Cette opération appartient au CORE ${operation.coreIp}. Revenez à cette adresse pour reprendre son suivi; aucun POST ne sera renvoyé.`);
      setRemovalModalOpen(true);
      return;
    }
    if (operation.phase === 'succeeded') {
      await finishRemovalSuccess(operation);
      return;
    }
    if (operation.phase === 'failed') {
      await finishRemovalFailure(operation);
      return;
    }
    const controller = new AbortController();
    removalTracker.current = controller;
    setRemovalLoading(true);
    setRemovalModalOpen(true);
    setRemovalMessage(operation.phase === 'submitting' ?
      'Reprise du suivi après envoi possible. La demande ne sera pas renvoyée.' :
      'Consultation de l’état de retrait…');
    try {
      const result = await pollRemovalOperation(operation.operationId,
        () => getRemovalOperation(ip, operation.operationId, operation.deviceId,
          token, controller.signal), {
          signal: controller.signal,
          waitUntilActive: waitUntilRemovalAppActive,
          onOperation: (status) => {
            if (status.state === 'processing' || status.state === 'uncertain') {
              const phase: StoredRemovalPhase = status.state;
              applyRemoval({ ...operation, phase });
              setRemovalMessage(status.state === 'processing' ?
                'Retrait en cours sur le CORE. La lampe reste dans la liste jusqu’à confirmation.' :
                'Le CORE signale un résultat incertain. Le suivi continue sans renvoyer la demande.');
            }
          },
        });
      if (!controller.signal.aborted) await handleRemovalPoll(result, operation);
    } catch (failure) {
      if (!controller.signal.aborted) {
        const uncertain = { ...operation, phase: 'uncertain' as const };
        try { await persistRemoval(uncertain); } catch {
          applyRemoval(uncertain);
          setRemovalStorageError('Le suivi doit rester enregistré pour éviter de renvoyer le POST.');
        }
        setRemovalMessage(`Suivi interrompu : ${describeApiError(failure)} Le résultat reste incertain; aucune nouvelle demande ne sera envoyée.`);
      }
    } finally {
      if (removalTracker.current === controller) removalTracker.current = null;
      if (screenMounted.current) setRemovalLoading(false);
    }
  }, [applyRemoval, finishRemovalFailure, finishRemovalSuccess, handleRemovalPoll,
    ip, persistRemoval, token]);

  const confirmRemovalRequest = useCallback(async (lamp: LampRecord) => {
    if (!acquireRemovalAction(removalStartLock.current)) return;
    setRemovalStarting(true);
    if (removalRef.current || removalStorageError) {
      Alert.alert('Retrait déjà en cours', 'Résolvez ou vérifiez l’opération enregistrée avant de démarrer un autre retrait.');
      setRemovalStarting(false);
      releaseRemovalAction(removalStartLock.current);
      return;
    }
    if (!lamp.online) {
      Alert.alert('Lampe hors ligne', 'Reconnectez la lampe et son MAIN avant de demander son retrait.');
      setRemovalStarting(false);
      releaseRemovalAction(removalStartLock.current);
      return;
    }
    const feedback = commandFeedback[String(lamp.id)];
    if (isCommandBlockingRemoval(feedback?.phase)) {
      Alert.alert('Commande en cours ou incertaine', 'Vérifiez d’abord le résultat ON/OFF de cette lampe.');
      setRemovalStarting(false);
      releaseRemovalAction(removalStartLock.current);
      return;
    }

    setRemovalMessage('Préparation de la demande…');
    setRemovalModalOpen(true);
    let operationContext: PendingRemovalOperation | null = null;
    let postMayHaveBeenSent = false;
    try {
      const operationId = await createRemovalOperationId(Crypto.getRandomBytesAsync);
      const operation: PendingRemovalOperation = {
        operationId,
        deviceId: lamp.id,
        deviceName: lamp.name,
        coreIp: ip,
        phase: 'submitting',
      };
      operationContext = operation;
      // Persist before the one and only POST so a restart can resume by GET.
      await savePendingRemoval(operation);
      applyRemoval(operation);
      setRemovalLoading(true);
      postMayHaveBeenSent = true;
      let submitted: Awaited<ReturnType<typeof requestDeviceRemoval>>;
      try {
        submitted = await requestDeviceRemoval(ip, lamp.id, operationId, token);
      } catch (failure) {
        if (failure instanceof CoreApiError && failure.kind === 'submission_uncertain') {
          const uncertain = { ...operation, phase: 'uncertain' as const };
          try { await persistRemoval(uncertain); } catch {
            applyRemoval(uncertain);
            setRemovalStorageError('Le suivi doit rester enregistré pour éviter de renvoyer le POST.');
          }
          setRemovalMessage('La réponse du POST a été perdue. Le retrait a peut-être été reçu; aucun second POST ne sera envoyé.');
          setRemovalLoading(false);
          void followRemoval(uncertain);
          return;
        }
        try {
          await clearPendingRemoval();
          applyRemoval(null);
          setRemovalModalOpen(false);
          setRemovalLoading(false);
          Alert.alert('Retrait non démarré', `${describeApiError(failure)} Le CORE a refusé la demande; aucune suppression locale n’a été faite.`);
        } catch {
          const failed = { ...operation, phase: 'failed' as const };
          applyRemoval(failed);
          setRemovalStorageError('Le CORE a refusé le retrait, mais son état sécurisé n’a pas pu être effacé.');
          setRemovalMessage('La demande a été refusée. La lampe reste dans l’inventaire; réessayez après avoir effacé cet état terminal.');
          setRemovalLoading(false);
        }
        return;
      }

      if (submitted.operation.state === 'failed') {
        await finishRemovalFailure(operation, submitted.operation.result_code);
        return;
      }
      if (submitted.operation.state === 'succeeded') {
        await finishRemovalSuccess(operation);
        return;
      }
      if (submitted.operation.state === 'not_found') {
        const notFound = { ...operation, phase: 'not_found' as const };
        await persistRemoval(notFound);
        setRemovalMessage('Le CORE ne retrouve pas cette opération. Cela ne prouve pas que le POST n’a jamais été reçu. Aucun nouveau POST ne sera envoyé.');
        return;
      }
      const tracking = { ...operation, phase: submitted.operation.state };
      await persistRemoval(tracking);
      setRemovalMessage(submitted.operation.state === 'processing' ?
        'Demande acceptée. Le retrait est en cours; 202 ne signifie pas que la lampe est déjà retirée.' :
        'Le résultat initial est incertain. Le suivi continue sans renvoyer la demande.');
      setRemovalLoading(false);
      void followRemoval(tracking);
    } catch (failure) {
      setRemovalLoading(false);
      if (postMayHaveBeenSent && operationContext) {
        const uncertain = { ...operationContext, phase: 'uncertain' as const };
        try { await persistRemoval(uncertain); } catch {
          applyRemoval(uncertain);
          setRemovalStorageError('Le suivi doit rester enregistré pour éviter de renvoyer le POST.');
        }
        setRemovalModalOpen(true);
        setRemovalMessage(`Le POST a peut-être été reçu, mais sa suite n’a pas pu être enregistrée correctement : ${describeApiError(failure)} Aucun nouveau POST ne sera envoyé.`);
        void followRemoval(uncertain);
      } else {
        setRemovalModalOpen(false);
        Alert.alert('Retrait non démarré', `L’identifiant ou son enregistrement sécurisé n’a pas pu être préparé. Aucun POST n’a été envoyé. ${describeApiError(failure)}`);
      }
    } finally {
      setRemovalStarting(false);
      releaseRemovalAction(removalStartLock.current);
    }
  }, [applyRemoval, commandFeedback, finishRemovalFailure,
    finishRemovalSuccess, followRemoval, ip, persistRemoval, removalStorageError, token]);

  const requestRemovalConfirmation = useCallback((lamp: LampRecord) => {
    if (!lamp.online) {
      Alert.alert('Lampe hors ligne', 'Reconnectez la lampe et son MAIN avant de demander son retrait.');
      return;
    }
    if (old) {
      Alert.alert('Inventaire à actualiser', 'Actualisez l’inventaire avant de retirer une lampe.');
      return;
    }
    if (isCommandBlockingRemoval(commandFeedback[String(lamp.id)]?.phase)) {
      Alert.alert('Commande en cours ou incertaine', 'Vérifiez d’abord le résultat ON/OFF de cette lampe.');
      return;
    }
    if (removalRef.current || removalStorageError) {
      Alert.alert('Retrait déjà en cours', 'Vérifiez l’opération enregistrée avant de démarrer un autre retrait.');
      return;
    }
    const confirmation = createRemovalConfirmation(lamp.name, lamp.id);
    Alert.alert(confirmation.title, confirmation.message, [
        { text: 'Annuler', style: 'cancel' },
        { text: 'Confirmer le retrait', style: 'destructive',
          onPress: () => void confirmRemovalRequest(lamp) },
      ]);
  }, [commandFeedback, confirmRemovalRequest, old, removalStorageError]);

  useEffect(() => {
    screenMounted.current = true;
    const requestVersionRef = requestVersion;
    const detailVersionRef = detailVersion;
    const startupRefresh = setTimeout(() => void refresh(), 0);
    const interval = setInterval(() => {
      if (AppState.currentState === 'active') void refresh();
    }, POLL_INTERVAL_MS);
    const subscription = AppState.addEventListener('change', (state) => {
      if (state === 'active') {
        setRevalidating(true);
        void refresh();
      } else {
        requestVersion.current++;
        setRevalidating(true);
      }
    });
    return () => {
      screenMounted.current = false;
      requestVersionRef.current++;
      detailVersionRef.current++;
      clearTimeout(startupRefresh);
      clearInterval(interval);
      subscription.remove();
      removalTracker.current?.abort();
      removalTracker.current = null;
    };
  }, [refresh]);

  useEffect(() => {
    let active = true;
    loadPendingRemoval().then((operation) => {
      if (!active || !operation) return;
      applyRemoval(operation);
      setRemovalModalOpen(true);
      if (operation.coreIp !== ip) {
        setRemovalMessage(`Cette opération appartient au CORE ${operation.coreIp}. Revenez à cette adresse pour reprendre son suivi; aucun POST ne sera renvoyé.`);
      } else if (operation.phase === 'succeeded') {
        void finishRemovalSuccess(operation);
      } else if (operation.phase === 'failed') {
        void finishRemovalFailure(operation);
      } else {
        void followRemoval(operation);
      }
    }).catch((failure) => {
      if (!active) return;
      const message = failure instanceof Error ? failure.message :
        'Impossible de lire le suivi sécurisé du retrait.';
      setRemovalStorageError(message);
      setRemovalMessage(message);
    });
    return () => {
      active = false;
      removalTracker.current?.abort();
      removalTracker.current = null;
    };
  }, [applyRemoval, finishRemovalFailure, finishRemovalSuccess, followRemoval, ip]);

  async function openDetail(id: V7Id) {
    const version = ++detailVersion.current;
    setSelectedId(id);
    setDetail(null);
    setDetailError('');
    setDetailLoading(true);
    try {
      const response = await getDevice(ip, id);
      if (version !== detailVersion.current) return;
      const lamp = lampsOnly([response.device])[0];
      if (!lamp) throw new Error('Réponse de lampe inattendue.');
      setDetail(lamp);
    } catch (failure) {
      if (version === detailVersion.current) setDetailError(describeApiError(failure));
    } finally {
      if (version === detailVersion.current) setDetailLoading(false);
    }
  }

  async function requestPower(id: V7Id, state: PowerState) {
    if (isRemovalBlockingCommand(id, removalRef.current?.deviceId)) return;
    const lamp = await onPower(id, state);
    if (!lamp || !screenMounted.current || lamp.id !== id) return;
    setSnapshot((current) => current ? {
      ...current,
      lamps: current.lamps.map((item) => item.id === id ? lamp : item),
      receivedAt: Date.now(),
    } : current);
    setDetail((current) => current?.id === id ? lamp : current);
    if (selectedId === id) setDetailError('');
  }

  function closeDetail() {
    detailVersion.current++;
    setSelectedId(null);
  }

  function closeRemovalModal() {
    setRemovalModalOpen(false);
    setSelectedId(null);
  }

  function openPairing() {
    if (removalRef.current || removalStartLock.current.active || removalStorageError) {
      Alert.alert('Retrait à vérifier', 'Terminez ou vérifiez le retrait enregistré avant de démarrer un appairage.');
      return;
    }
    setPairingOpen(true);
  }

  function resumeRemoval(operation: PendingRemovalOperation) {
    setRemovalModalOpen(true);
    if (operation.phase === 'succeeded') void finishRemovalSuccess(operation);
    else if (operation.phase === 'failed') void finishRemovalFailure(operation);
    else void followRemoval(operation);
  }

  function removalPhaseLabel(operation: PendingRemovalOperation): string {
    if (operation.phase === 'submitting') return 'Demande préparée';
    if (operation.phase === 'processing') return 'Retrait en cours';
    if (operation.phase === 'succeeded') return 'Retrait confirmé';
    if (operation.phase === 'failed') return 'Retrait échoué';
    if (operation.phase === 'not_found') return 'Opération introuvable';
    return 'Résultat incertain';
  }

  const onlineModules = snapshot?.modules.filter((module) => module.online).length ?? 0;
  const detailMainOnline = Boolean(detail && snapshot?.modules.some((module) =>
    module.id === detail.parent_id && module.online));
  const removalCommandBlocked = Boolean(detail &&
    isCommandBlockingRemoval(commandFeedback[String(detail.id)]?.phase));
  const removalActionDisabled = !canRequestRemoval({
    lampOnline: Boolean(detail?.online),
    mainOnline: detailMainOnline,
    inventoryFresh: !old,
    commandPhase: detail ? commandFeedback[String(detail.id)]?.phase : undefined,
    hasPendingRemoval: Boolean(removal),
    storageAvailable: !removalStorageError,
    actionLocked: removalStarting,
  });

  return (
    <View style={styles.root}>
      <ScrollView contentContainerStyle={styles.content}
        refreshControl={<RefreshControl refreshing={refreshing}
          onRefresh={() => void refresh(true)} tintColor="#18785d" />}>
        <View style={styles.header}>
          <View style={styles.brand}><Text style={styles.brandText}>SL</Text></View>
          <Pressable onPress={onSettings} style={styles.settings} accessibilityRole="button"
            accessibilityLabel="Paramètres de connexion">
            <Text style={styles.settingsText}>Réglages</Text>
          </Pressable>
        </View>
        <Text style={styles.eyebrow}>MAISON · ÉCLAIRAGE</Text>
        <Text style={styles.title}>Mes lampes</Text>
        <Text style={styles.subtitle}>CORE-WIFI · {ip}</Text>

        <View style={styles.coreCard}>
          <View style={styles.coreTop}>
            <Text style={styles.coreLabel}>CORE</Text>
            <StatusIndicator label={revalidating ? 'Vérification' : coreReachable === true ?
              'Connecté' : coreReachable === false ? 'Inaccessible' : 'Chargement'}
              tone={revalidating ? 'warn' : coreReachable === true ? 'good' :
                coreReachable === false ? 'bad' : 'neutral'} onDark />
          </View>
          <Text style={styles.coreTitle}>{revalidating ? 'Vérification en cours' :
            error && coreReachable ? 'Inventaire indisponible' :
              error ? 'Connexion interrompue' : snapshot ? 'Système joignable' : 'Connexion en cours…'}</Text>
          <Text style={styles.coreCaption}>{error ? error : snapshot ?
            `${onlineModules}/${snapshot.modules.length} MAIN en ligne · UART ${snapshot.health.uart_driver_ready ? 'prêt' : 'non prêt'}` :
            'Lecture de l’inventaire du CORE'}</Text>
          <Text style={styles.coreFoot}>La disponibilité HTTP ne confirme pas la liaison Zigbee.</Text>
        </View>

        <View style={styles.sectionHeader}>
          <View>
            <Text style={styles.sectionTitle}>Équipements</Text>
            <Text style={styles.sectionCaption}>{snapshot ? `${snapshot.lamps.length} lampe${snapshot.lamps.length > 1 ? 's' : ''}` : 'Inventaire en attente'}</Text>
          </View>
          <Pressable onPress={() => void refresh(true)} disabled={refreshing}
            accessibilityRole="button" accessibilityLabel="Actualiser les lampes">
            <Text style={styles.refresh}>Actualiser</Text>
          </Pressable>
        </View>

        <Pressable onPress={openPairing}
          disabled={Boolean(removal) || Boolean(removalStorageError) || removalStarting}
          style={[styles.addLampButton, (removal || removalStorageError || removalStarting) && styles.buttonDim]}
          accessibilityRole="button" accessibilityLabel="Ajouter une lampe">
          <Text style={styles.addLampText}>＋ Ajouter une lampe</Text>
        </Pressable>

        {removal ? <View style={styles.removalBanner}>
          <Text style={styles.removalBannerTitle}>{removalPhaseLabel(removal)} · {removal.deviceName}</Text>
          <Text style={styles.removalBannerText}>ID V7 {removal.deviceId} · opération {removal.operationId}</Text>
          <Pressable onPress={() => resumeRemoval(removal)} disabled={removalLoading}
            accessibilityRole="button" accessibilityLabel="Reprendre le suivi du retrait">
            <Text style={styles.removalBannerAction}>{removal.phase === 'succeeded' ?
              'Actualiser l’inventaire' : removal.phase === 'failed' ?
                'Nettoyer le résultat' : isRemovalInProgress(removal.phase) ?
                  'Ouvrir le suivi' : 'Vérifier le même identifiant'}</Text>
          </Pressable>
        </View> : null}
        {removalStorageError && !removal ? <View style={styles.removalWarning}>
          <Text style={styles.removalWarningText}>{removalStorageError} Aucun nouveau retrait ne sera envoyé tant que cet état n’est pas résolu.</Text>
        </View> : null}

        {snapshot && old ? <View style={styles.staleBanner}>
          <Text style={styles.staleText}>Dernières données reçues. La disponibilité et les confirmations ne sont plus actualisées.</Text>
        </View> : null}

        {loading && !snapshot ? <ActivityIndicator size="large" color="#18785d"
          style={styles.loader} /> : null}
        {!loading && !snapshot ? <View style={styles.empty}>
          <Text style={styles.emptyTitle}>Inventaire indisponible</Text>
          <Text style={styles.emptyText}>Vérifiez l’adresse, le Wi-Fi du téléphone et CORE-WIFI, puis actualisez.</Text>
        </View> : null}
        {snapshot && snapshot.lamps.length === 0 ? <View style={styles.empty}>
          <Text style={styles.emptyTitle}>Aucune lampe découverte</Text>
          <Text style={styles.emptyText}>Le CORE ne référence encore aucun équipement de rôle lamp.</Text>
        </View> : null}

        {snapshot?.lamps.map((lamp) => <LampCard key={lamp.id} lamp={lamp}
          mainOnline={snapshot.modules.some((module) =>
            module.id === lamp.parent_id && module.online)}
          snapshotOld={old} feedback={commandFeedback[String(lamp.id)]}
          powerBlockedByRemoval={isRemovalBlockingCommand(lamp.id, removal?.deviceId) ||
            (removalStarting && selectedId === lamp.id)}
          onPower={(state) => void requestPower(lamp.id, state)}
          onPress={() => void openDetail(lamp.id)} />)}
        {snapshot ? <Text style={styles.updated}>Dernière lecture · {new Date(snapshot.receivedAt).toLocaleTimeString()}</Text> : null}
        <Text style={styles.footer}>L’état confirmé est mis à jour après ACK et relecture du CORE. Aucune mesure électrique réelle n’est disponible.</Text>
      </ScrollView>

      <Modal visible={selectedId !== null || removalModalOpen} animationType="slide" transparent
        onRequestClose={removalModalOpen ? closeRemovalModal : closeDetail}>
        <View style={styles.modalBackdrop}>
          <View style={styles.modalCard}>
            <View style={styles.modalHeader}>
              <Text style={styles.modalTitle}>{removalModalOpen ? 'Suivi du retrait' : 'Détail de la lampe'}</Text>
              <Pressable onPress={removalModalOpen ? closeRemovalModal : closeDetail}
                accessibilityRole="button" accessibilityLabel="Fermer">
                <Text style={styles.close}>Fermer</Text></Pressable>
            </View>
            {removalModalOpen && removal ? <>
              <Text style={styles.detailName}>{removal.deviceName}</Text>
              <Text style={styles.detailLine}>ID V7 : {removal.deviceId}</Text>
              <Text style={styles.detailLine}>Opération : {removal.operationId}</Text>
              <Text style={styles.detailLine}>État : {removalPhaseLabel(removal)}</Text>
              {removalLoading ? <ActivityIndicator color="#18785d" style={styles.loader} /> : null}
              {removalMessage ? <Text style={removal.phase === 'failed' ?
                styles.errorText : styles.removalMessage}>{removalMessage}</Text> : null}
              <Text style={styles.detailNote}>Le retrait enlève l’association Smart Lighting; il ne commande pas l’extinction électrique. Aucun POST ne sera répété automatiquement.</Text>
              <Pressable onPress={() => resumeRemoval(removal)} disabled={removalLoading}
                style={[styles.removalAction, removalLoading && styles.buttonDim]}
                accessibilityRole="button" accessibilityLabel={removal.phase === 'succeeded' ?
                  'Actualiser l’inventaire après le retrait' : 'Vérifier à nouveau le retrait'}>
                <Text style={styles.removalActionText}>{removal.phase === 'succeeded' ?
                  'Actualiser l’inventaire' : removal.phase === 'failed' ?
                    'Nettoyer le résultat' : 'Vérifier à nouveau'}</Text>
              </Pressable>
            </> : null}
            {removalModalOpen && !removal ? <>
              <ActivityIndicator color="#18785d" style={styles.loader} />
              <Text style={styles.removalMessage}>{removalMessage || 'Préparation du retrait…'}</Text>
            </> : null}
            {!removalModalOpen && detailLoading ? <ActivityIndicator color="#18785d" style={styles.loader} /> : null}
            {!removalModalOpen && detailError ? <Text style={styles.errorText}>{detailError}</Text> : null}
            {!removalModalOpen && detail ? <>
              <Text style={styles.detailName}>{detail.name}</Text>
              <Text style={styles.detailLine}>ID V7 : {detail.id}</Text>
              <Text style={styles.detailLine}>MAIN parent : {detail.parent_id}</Text>
              <Text style={styles.detailLine}>Disponibilité : {detail.online ? 'en ligne' : 'hors ligne'}</Text>
              <Text style={styles.detailLine}>Dernier état confirmé : {detail.last_confirmed_state.power?.toUpperCase() ?? 'inconnu'}</Text>
              <Text style={styles.detailLine}>Statut : {detail.last_confirmed_state.status}</Text>
              <Text style={styles.detailNote}>Historique logiciel reçu du CORE. La lampe ne fournit pas de mesure électrique.</Text>
              <Pressable onPress={() => requestRemovalConfirmation(detail)}
                disabled={removalActionDisabled}
                style={[styles.removalDangerButton, removalActionDisabled && styles.buttonDim]}
                accessibilityRole="button" accessibilityLabel="Retirer cette lampe">
                <Text style={styles.removalDangerText}>Retirer cette lampe</Text>
              </Pressable>
              {removalActionDisabled ? <Text style={styles.disabledReason}>
                {!detail.online ? 'Lampe hors ligne.' : !detailMainOnline ? 'MAIN indisponible.' :
                  old ? 'Actualisez les données avant le retrait.' :
                    removalCommandBlocked ? 'Vérifiez d’abord la commande ON/OFF.' :
                      removalStorageError || 'Un autre retrait est à vérifier.'}
              </Text> : null}
            </> : null}
          </View>
        </View>
      </Modal>
      {pairingOpen ? <PairingScreen ip={ip} token={token}
        onClose={() => setPairingOpen(false)} onInventoryRefresh={() => void refresh(true)} /> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#f4f7f4' },
  content: { padding: 22, paddingTop: 22, paddingBottom: 42 },
  header: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 30 },
  brand: { width: 43, height: 43, backgroundColor: '#18785d', borderRadius: 14,
    justifyContent: 'center', alignItems: 'center' },
  brandText: { color: '#fff', fontSize: 15, fontWeight: '900' },
  settings: { backgroundColor: '#fff', borderColor: '#dde6e0', borderWidth: 1,
    paddingHorizontal: 14, paddingVertical: 10, borderRadius: 12 },
  settingsText: { color: '#476256', fontSize: 12, fontWeight: '700' },
  eyebrow: { color: '#17805f', fontSize: 11, fontWeight: '800', letterSpacing: 2 },
  title: { color: '#172b36', fontSize: 32, fontWeight: '800', marginTop: 8 },
  subtitle: { color: '#7a8a91', fontSize: 13, marginTop: 4 },
  coreCard: { backgroundColor: '#183c35', borderRadius: 22, padding: 20, marginTop: 26 },
  coreTop: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  coreLabel: { color: '#a6d6be', fontSize: 11, fontWeight: '800', letterSpacing: 1.8 },
  coreTitle: { color: '#fff', fontSize: 21, fontWeight: '800', marginTop: 18 },
  coreCaption: { color: '#c1d7cf', fontSize: 13, marginTop: 6, lineHeight: 19 },
  coreFoot: { color: '#a7c5b8', fontSize: 10, marginTop: 16 },
  sectionHeader: { flexDirection: 'row', justifyContent: 'space-between',
    alignItems: 'center', marginTop: 29, marginBottom: 16 },
  sectionTitle: { color: '#172b36', fontSize: 20, fontWeight: '800' },
  sectionCaption: { color: '#8a999f', fontSize: 12, marginTop: 3 },
  refresh: { color: '#18785d', fontSize: 13, fontWeight: '800', padding: 8 },
  addLampButton: { minHeight: 48, alignItems: 'center', justifyContent: 'center',
    backgroundColor: '#18785d', borderRadius: 14, marginBottom: 14 },
  addLampText: { color: '#fff', fontSize: 14, fontWeight: '800' },
  removalBanner: { backgroundColor: '#fff4e8', borderColor: '#f0d4b6', borderWidth: 1,
    padding: 14, borderRadius: 14, marginBottom: 14 },
  removalBannerTitle: { color: '#70421d', fontSize: 13, fontWeight: '800' },
  removalBannerText: { color: '#825c3b', fontSize: 11, marginTop: 5 },
  removalBannerAction: { color: '#9b4d2e', fontSize: 12, fontWeight: '800', marginTop: 10 },
  removalWarning: { backgroundColor: '#fff0d8', padding: 13, borderRadius: 12,
    marginBottom: 14 },
  removalWarningText: { color: '#825718', fontSize: 12, lineHeight: 18 },
  staleBanner: { backgroundColor: '#fff0d8', padding: 13, borderRadius: 12, marginBottom: 14 },
  staleText: { color: '#825718', fontSize: 12, lineHeight: 18 },
  loader: { marginVertical: 30 },
  empty: { padding: 22, backgroundColor: '#fff', borderRadius: 18 },
  emptyTitle: { color: '#273c45', fontSize: 16, fontWeight: '700' },
  emptyText: { color: '#75868c', fontSize: 13, marginTop: 7, lineHeight: 20 },
  updated: { color: '#82929a', fontSize: 11, textAlign: 'center', marginTop: 5 },
  footer: { color: '#9aa8ad', fontSize: 11, lineHeight: 17, textAlign: 'center', marginTop: 24 },
  modalBackdrop: { flex: 1, backgroundColor: 'rgba(19,36,34,0.45)', justifyContent: 'flex-end' },
  modalCard: { backgroundColor: '#fff', borderTopLeftRadius: 24, borderTopRightRadius: 24,
    padding: 24, paddingBottom: 42, maxHeight: '92%' },
  modalHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  modalTitle: { color: '#172b36', fontSize: 18, fontWeight: '800' },
  close: { color: '#18785d', fontSize: 13, fontWeight: '700' },
  buttonDim: { opacity: 0.5 },
  disabledReason: { color: '#89959b', fontSize: 11, lineHeight: 17, marginTop: 9 },
  detailName: { color: '#172b36', fontSize: 24, fontWeight: '800', marginTop: 24, marginBottom: 10 },
  detailLine: { color: '#4a626c', fontSize: 14, marginTop: 8 },
  detailNote: { color: '#82929a', fontSize: 12, lineHeight: 18, marginTop: 22 },
  removalMessage: { color: '#526c77', fontSize: 13, lineHeight: 20, marginTop: 18 },
  removalDangerButton: { minHeight: 48, alignItems: 'center', justifyContent: 'center',
    borderWidth: 1, borderColor: '#e2b5ae', backgroundColor: '#fff5f3',
    borderRadius: 13, marginTop: 20 },
  removalDangerText: { color: '#a43f38', fontSize: 14, fontWeight: '800' },
  removalAction: { minHeight: 46, alignItems: 'center', justifyContent: 'center',
    backgroundColor: '#edf4f0', borderRadius: 12, marginTop: 18 },
  removalActionText: { color: '#236149', fontSize: 13, fontWeight: '800' },
  errorText: { color: '#b64e49', fontSize: 13, marginTop: 20 },
});
