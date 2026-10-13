import { useCallback, useEffect, useRef, useState } from 'react';
import { ActivityIndicator, Alert, AppState, Modal, Pressable, SafeAreaView,
  ScrollView, StyleSheet, Text, TextInput, View } from 'react-native';
import { CoreApiError, closePairingSession, describeApiError, getDevices,
  getHealth, getPairingCandidates, getPairingOperation, getPairingSession,
  lampsOnly, requestPairingOperation, startPairingSession } from '../../api/coreApiClient';
import type { PairingCandidate, PairingOperationRequest,
  PairingOperationState, PairingOperationSummary, PairingSession, V7Id } from '../../api/types';
import { StatusIndicator } from '../../components/StatusIndicator';
import { acquirePairingAction, closePairingSessionOnce, createPairingOperationId,
  findNewlyPairedLamp, findPairingCandidate, openPairingSessionOnce,
  isValidPairingName,
  pollPairingOperation, pollPairingSession,
  retainPairingSelection,
  releasePairingAction, submitPairingOperationOnce } from './pairingWorkflow';

const SEARCH_DURATION_SECONDS = 60;
const CANDIDATE_REFRESH_MS = 3000;
const INVENTORY_SYNC_ATTEMPTS = 10;

interface Props {
  ip: string;
  token: string;
  onClose: () => void;
  onInventoryRefresh: () => void;
}

function waitUntilAppActive(signal?: AbortSignal): Promise<void> {
  const current = AppState.currentState;
  if (!signal || signal.aborted || (current !== 'background' && current !== 'inactive')) {
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
    if (AppState.currentState === 'active') finish();
  });
}

function sessionLabel(state?: PairingSession['state']): string {
  switch (state) {
    case 'opening': return 'Ouverture en cours';
    case 'active': return 'Recherche active';
    case 'closing': return 'Fermeture en cours';
    case 'closed': return 'Recherche fermée';
    case 'expired': return 'Recherche expirée';
    case 'failed': return 'Échec de la session';
    case 'uncertain': return 'État de session incertain';
    default: return 'Aucune session';
  }
}

function operationLabel(state?: PairingOperationState): string {
  switch (state) {
    case 'pending': return 'En attente';
    case 'processing': return 'Appairage en cours';
    case 'succeeded': return 'Appairage confirmé';
    case 'failed': return 'Appairage échoué';
    case 'rejected': return 'Appairage refusé';
    case 'expired': return 'Résultat expiré';
    case 'uncertain': return 'Résultat incertain';
    default: return '';
  }
}

function operationIsTerminal(state?: PairingOperationState): boolean {
  return state === 'succeeded' || state === 'failed' || state === 'rejected' ||
    state === 'expired' || state === 'uncertain';
}

function operationResultMessage(code?: string): string | null {
  switch (code) {
    case 'capacity': return 'La capacité disponible est atteinte.';
    case 'route_unavailable': return 'Le CORE ne dispose pas d’une route Zigbee vers cette candidate.';
    case 'not_found': return 'La candidate n’est plus détectée par le MAIN.';
    case 'busy': return 'Le MAIN traite déjà une autre opération.';
    case 'rejected': return 'Le MAIN a refusé l’appairage.';
    case 'expired': return 'Le MAIN n’a pas terminé l’appairage dans le délai prévu.';
    case 'uncertain': return 'Le résultat de l’appairage ne peut pas être confirmé.';
    case 'failed': return 'Le MAIN a signalé un échec d’appairage.';
    default: return null;
  }
}

export function PairingScreen({ ip, token, onClose, onInventoryRefresh }: Props) {
  const [loading, setLoading] = useState(true);
  const [reachable, setReachable] = useState<boolean | null>(null);
  const [refreshing, setRefreshing] = useState(false);
  const [session, setSession] = useState<PairingSession | null>(null);
  const [sessionOrigin, setSessionOrigin] = useState<'new' | 'existing' | null>(null);
  const [sessionUncertain, setSessionUncertain] = useState(false);
  const [remainingSeconds, setRemainingSeconds] = useState<number | null>(null);
  const [candidates, setCandidates] = useState<PairingCandidate[]>([]);
  const [capacityReached, setCapacityReached] = useState(false);
  const [selectedHardwareId, setSelectedHardwareId] = useState<string | null>(null);
  const [name, setName] = useState('');
  const [confirming, setConfirming] = useState(false);
  const [operation, setOperation] = useState<PairingOperationSummary | null>(null);
  const [operationPhase, setOperationPhase] = useState<
    'submitting' | 'tracking' | 'uncertain' | 'error' | null>(null);
  const [actionBusy, setActionBusy] = useState(false);
  const [operationError, setOperationError] = useState('');
  const [inventoryMessage, setInventoryMessage] = useState('');
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');

  const mounted = useRef(true);
  const sessionRef = useRef<PairingSession | null>(null);
  const selectedRef = useRef<string | null>(null);
  const actionLock = useRef({ active: false });
  const operationIdRef = useRef<V7Id | null>(null);
  const operationController = useRef<AbortController | null>(null);
  const inventoryIds = useRef<Set<V7Id> | null>(null);
  const countdownDeadline = useRef<number | null>(null);
  const refreshRef = useRef<() => Promise<void>>(async () => {});
  const resumeOperationRef = useRef<() => Promise<void>>(async () => {});

  const acquireAction = useCallback(() => {
    const acquired = acquirePairingAction(actionLock.current);
    if (acquired) setActionBusy(true);
    return acquired;
  }, []);
  const releaseAction = useCallback(() => {
    releasePairingAction(actionLock.current);
    if (mounted.current) setActionBusy(false);
  }, []);

  const applySession = useCallback((next: PairingSession | null) => {
    if (!mounted.current) return;
    if (sessionRef.current?.id !== next?.id) {
      setSelectedHardwareId(null);
      selectedRef.current = null;
      setName('');
      setConfirming(false);
      setCandidates([]);
    }
    sessionRef.current = next;
    setSession(next);
    if (!next || next.state !== 'active') {
      setCandidates([]);
      if (!next || next.state === 'closed' || next.state === 'expired' || next.state === 'failed') {
        setCapacityReached(false);
      }
    }
  }, []);

  const refreshState = useCallback(async (manual = false) => {
    if (manual) setRefreshing(true);
    let sessionRead = false;
    try {
      const response = await getPairingSession(ip, token);
      if (!mounted.current) return;
      sessionRead = true;
      setReachable(true);
      setError('');
      setSessionUncertain(false);
      applySession(response.session);
      if (response.session?.state === 'active') {
        const candidateResponse = await getPairingCandidates(ip, token);
        if (!mounted.current) return;
        if (candidateResponse.session_id !== response.session.id) {
          setCandidates([]);
          setError('La liste reçue appartient à une autre session. Relisez l’état du CORE.');
          return;
        }
        setCandidates(candidateResponse.candidates);
        setCapacityReached(candidateResponse.capacity_reached ||
          response.session.candidate_limit_reached === true);
        const selected = selectedRef.current;
        if (selected && !retainPairingSelection(candidateResponse.candidates, selected) &&
            !actionLock.current.active) {
          setSelectedHardwareId(null);
          selectedRef.current = null;
          setName('');
          setConfirming(false);
          setNotice('La candidate sélectionnée a disparu ou a été appairée. Choisissez à nouveau.');
        }
      }
    } catch (failure) {
      if (mounted.current) {
        if (!sessionRead) setReachable(false);
        setError(describeApiError(failure));
      }
    } finally {
      if (mounted.current && manual) setRefreshing(false);
    }
  }, [applySession, ip, token]);
  const refreshCandidates = useCallback(async (sessionId: V7Id) => {
    try {
      const response = await getPairingCandidates(ip, token);
      if (!mounted.current || sessionRef.current?.id !== sessionId ||
          sessionRef.current.state !== 'active') return;
      if (response.session_id !== sessionId) {
        setCandidates([]);
        setError('La liste reçue appartient à une autre session. Relisez l’état du CORE.');
        return;
      }
      setCandidates(response.candidates);
      setCapacityReached(response.capacity_reached ||
        sessionRef.current.candidate_limit_reached === true);
      const selected = selectedRef.current;
      if (selected && !retainPairingSelection(response.candidates, selected) &&
          !actionLock.current.active) {
        setSelectedHardwareId(null);
        selectedRef.current = null;
        setName('');
        setConfirming(false);
        setNotice('La candidate sélectionnée a disparu ou a été appairée. Choisissez à nouveau.');
      }
    } catch (failure) {
      if (mounted.current) setError(describeApiError(failure));
    }
  }, [ip, token]);

  const readInitialState = useCallback(async () => {
    setLoading(true);
    setError('');
    let healthRead = false;
    try {
      await getHealth(ip);
      if (!mounted.current) return;
      healthRead = true;
      setReachable(true);
      const [sessionResult, devicesResult] = await Promise.allSettled([
        getPairingSession(ip, token), getDevices(ip),
      ]);
      if (!mounted.current) return;
      if (sessionResult.status === 'rejected') throw sessionResult.reason;
      const current = sessionResult.value.session;
      setSessionOrigin(current ? 'existing' : null);
      applySession(current);
      setSessionUncertain(false);
      if (devicesResult.status === 'fulfilled') {
        inventoryIds.current = new Set(lampsOnly(devicesResult.value.devices).map((lamp) => lamp.id));
      } else {
        inventoryIds.current = null;
        setInventoryMessage('Inventaire initial indisponible; il sera relu après l’appairage.');
      }
      if (current?.state === 'active') {
        await refreshCandidates(current.id);
        setNotice('Session existante reprise. Aucune nouvelle session n’a été ouverte.');
      } else if (current?.state === 'opening') {
        setNotice('Session existante en cours d’ouverture; attente de la confirmation du CORE.');
      }
    } catch (failure) {
      if (mounted.current) {
        setReachable(healthRead);
        setError(describeApiError(failure));
      }
    } finally {
      if (mounted.current) setLoading(false);
    }
  }, [applySession, ip, refreshCandidates, token]);

  const syncInventory = useCallback(async (lampName?: string, knownIds?: Set<V7Id> | null) => {
    let found = false;
    for (let attempt = 0; attempt < INVENTORY_SYNC_ATTEMPTS; attempt++) {
      await new Promise((resolve) => setTimeout(resolve, 1000));
      if (!mounted.current || AppState.currentState !== 'active') continue;
      try {
        const response = await getDevices(ip);
        const newLamp = findNewlyPairedLamp(response.devices, knownIds ?? null, lampName);
        if (newLamp) {
          found = true;
          setInventoryMessage(`Lampe « ${newLamp.name} » visible dans l’inventaire.`);
          onInventoryRefresh();
          break;
        }
      } catch {
        // The pairing result remains authoritative; inventory synchronization is retried below.
      }
    }
    if (mounted.current && !found) {
      setInventoryMessage('Appairage confirmé, synchronisation de l’inventaire en cours. Actualisez la liste dans quelques instants.');
      onInventoryRefresh();
    }
  }, [ip, onInventoryRefresh]);

  const applyOperation = useCallback((next: PairingOperationSummary) => {
    if (!mounted.current) return;
    setOperation(next);
    setOperationPhase('tracking');
    setOperationError('');
    if (operationIsTerminal(next.state)) {
      setOperationPhase(null);
      if (next.state === 'succeeded') {
        setSelectedHardwareId(null);
        selectedRef.current = null;
        setName('');
        setConfirming(false);
        setInventoryMessage('Appairage confirmé, synchronisation de l’inventaire en cours.');
        onInventoryRefresh();
        void syncInventory(next.name, inventoryIds.current);
      }
    }
  }, [onInventoryRefresh, syncInventory]);

  const followOperation = useCallback(async (id: V7Id, lockAlreadyHeld = false) => {
    if (!lockAlreadyHeld && !acquireAction()) return;
    setOperationPhase('tracking');
    setOperationError('');
    const controller = new AbortController();
    operationController.current?.abort();
    operationController.current = controller;
    try {
      const result = await pollPairingOperation(id,
        () => getPairingOperation(ip, id, token, controller.signal), {
          signal: controller.signal,
          waitUntilActive: waitUntilAppActive,
        });
      if (!mounted.current || controller.signal.aborted) return;
      if (result.kind === 'ready') {
        applyOperation(result.value);
      } else if (result.kind === 'not_found') {
        setOperationPhase('uncertain');
        setOperationError('Le CORE ne conserve plus cette opération. Son résultat reste incertain; aucun nouvel envoi ne sera fait automatiquement.');
      } else if (result.kind === 'timeout') {
        setOperationPhase('uncertain');
        setOperationError('Le suivi a dépassé le délai mobile. Vérifiez de nouveau l’opération avant toute autre demande.');
      } else if (result.kind === 'unavailable') {
        setOperationPhase('uncertain');
        setOperationError(`Suivi interrompu : ${describeApiError(result.error)} Résultat incertain.`);
      }
    } finally {
      if (operationController.current === controller) operationController.current = null;
      if (lockAlreadyHeld || actionLock.current.active) releaseAction();
    }
  }, [acquireAction, applyOperation, ip, releaseAction, token]);

  useEffect(() => {
    refreshRef.current = () => refreshState(false);
    resumeOperationRef.current = async () => {
      const id = operationIdRef.current;
      if (!id || (operation && operationIsTerminal(operation.state))) return;
      await followOperation(id);
    };
  }, [followOperation, operation, refreshState]);

  useEffect(() => {
    mounted.current = true;
    const initialLoad = setTimeout(() => void readInitialState(), 0);
    const interval = setInterval(() => {
      if (AppState.currentState === 'active') void refreshRef.current();
    }, CANDIDATE_REFRESH_MS);
    const subscription = AppState.addEventListener('change', (state) => {
      if (state === 'active') {
        void refreshRef.current();
        void resumeOperationRef.current();
      }
    });
    return () => {
      mounted.current = false;
      operationController.current?.abort();
      clearTimeout(initialLoad);
      clearInterval(interval);
      subscription.remove();
    };
  }, [readInitialState]);

  const sessionId = session?.id;
  const sessionState = session?.state;
  useEffect(() => {
    if (sessionState !== 'active' || sessionOrigin !== 'new' ||
        countdownDeadline.current === null) return undefined;
    const update = () => setRemainingSeconds(Math.max(0,
      Math.ceil((countdownDeadline.current! - Date.now()) / 1000)));
    const timer = setInterval(update, 1000);
    return () => clearInterval(timer);
  }, [sessionId, sessionState, sessionOrigin]);

  async function startSearch() {
    if (!acquireAction()) return;
    setRefreshing(true);
    setError('');
    setNotice('Demande de recherche envoyée; attente de la confirmation du CORE.');
    try {
      const result = await openPairingSessionOnce(
        () => startPairingSession(ip, SEARCH_DURATION_SECONDS, token),
        () => getPairingSession(ip, token), {
          timeoutMs: 10000,
          onSession: applySession,
          waitUntilActive: waitUntilAppActive,
        });
      if (result.kind === 'ready') {
        setSessionOrigin('new');
        const duration = result.value.duration_seconds ?? SEARCH_DURATION_SECONDS;
        countdownDeadline.current = Date.now() + duration * 1000;
        setRemainingSeconds(duration);
        setSessionUncertain(false);
        setNotice('Recherche confirmée active par le CORE.');
        await refreshCandidates(result.value.id);
      } else if (result.kind === 'terminal') {
        setSessionOrigin('existing');
        setNotice(`La session a terminé son ouverture avec l’état « ${result.value?.state ?? 'inconnu'} ».`);
      } else if (result.kind === 'none') {
        setSessionUncertain(true);
        setNotice('Aucune session visible après la demande. Son résultat est incertain; actualisez son état avant une nouvelle recherche.');
      } else if (result.kind === 'timeout') {
        setSessionUncertain(true);
        setNotice('Le CORE n’a pas confirmé l’ouverture à temps. Relisez la session; aucune seconde demande n’a été envoyée.');
      } else if (result.kind === 'unavailable') {
        setSessionUncertain(true);
        setError(describeApiError(result.error));
      }
    } catch (failure) {
      setError(describeApiError(failure));
      if (failure instanceof CoreApiError && failure.kind === 'submission_uncertain') {
        setSessionUncertain(true);
        setNotice('Réponse d’ouverture perdue. Relisez la session avant toute nouvelle recherche.');
      } else {
        void refreshState(true);
      }
    } finally {
      setRefreshing(false);
      releaseAction();
    }
  }

  async function confirmPairing() {
    const currentSession = sessionRef.current;
    const candidate = findPairingCandidate(candidates, selectedHardwareId);
    const trimmedName = name.trim();
    if (!currentSession || currentSession.state !== 'active' || !candidate ||
        !isValidPairingName(trimmedName)) {
      setError('La session, la candidate ou le nom n’est plus valide. Actualisez et vérifiez votre sélection.');
      return;
    }
    if (operationPhase === 'uncertain' || operation?.state === 'uncertain' ||
        operation?.state === 'expired') {
      setError('Le résultat de l’opération précédente doit être résolu avant un nouvel appairage.');
      return;
    }
    if (!acquireAction()) return;
    setError('');
    setOperationError('');
    setOperationPhase('submitting');
    setInventoryMessage('');
    const operationId = createPairingOperationId();
    operationIdRef.current = operationId;
    const request: PairingOperationRequest = {
      session_id: currentSession.id,
      operation_id: operationId,
      hardware_id: candidate.hardware_id,
      name: trimmedName,
    };
    setOperation({ ...request, id: operationId, state: 'pending' });

    let submitted;
    try {
      submitted = await submitPairingOperationOnce(request,
        (body) => requestPairingOperation(ip, body, token),
        (id) => getPairingOperation(ip, id, token));
    } catch (failure) {
      setOperation(null);
      operationIdRef.current = null;
      setOperationPhase('error');
      setError(describeApiError(failure));
      releaseAction();
      return;
    }
    if (submitted.kind === 'uncertain') {
      setOperationPhase('uncertain');
      setOperationError(`Réponse d’appairage perdue. ${describeApiError(submitted.error)} Aucun POST ne sera répété.`);
      releaseAction();
      return;
    }
    if (submitted.kind === 'cancelled') {
      setOperationPhase('uncertain');
      setOperationError('Suivi interrompu. Vérifiez l’opération avant toute nouvelle demande.');
      releaseAction();
      return;
    }
    setOperation({ ...submitted.operation, name: trimmedName });
    setOperationPhase('tracking');
    setConfirming(false);
    await followOperation(operationId, true);
  }

  async function checkOperationAgain() {
    const id = operationIdRef.current;
    if (!id) return;
    await followOperation(id);
  }

  async function requestClose(confirmUncertain = false) {
    if (actionLock.current.active) {
      Alert.alert('Opération en cours', 'Attendez son résultat avant de quitter la session.');
      return;
    }
    if (!confirmUncertain && (operationPhase === 'uncertain' ||
        operation?.state === 'uncertain' || operation?.state === 'expired')) {
      Alert.alert('Résultat d’appairage non confirmé',
        'Fermer la session ne confirme pas le résultat de l’opération. Consultez son état avant de quitter.', [
          { text: 'Rester', style: 'cancel' },
          { text: 'Vérifier', onPress: () => void checkOperationAgain() },
          { text: 'Fermer la session', style: 'destructive',
            onPress: () => void requestClose(true) },
        ]);
      return;
    }
    const current = sessionRef.current;
    if (!current) {
      if (sessionUncertain) {
        Alert.alert('État de session incertain',
          'Le CORE ne montre pas de session, mais la réponse d’ouverture a été perdue. Vous pouvez relire son état ou quitter sans relancer de recherche.', [
            { text: 'Rester', style: 'cancel' },
            { text: 'Relire', onPress: () => void refreshState(true) },
            { text: 'Quitter', style: 'destructive', onPress: onClose },
          ]);
      } else onClose();
      return;
    }
    if (current.state === 'closed' || current.state === 'expired' || current.state === 'failed') {
      onClose();
      return;
    }
    if (!acquireAction()) return;
    setRefreshing(true);
    setError('');
    try {
      let result;
      if (current.state === 'closing') {
        result = await pollPairingSession(() => getPairingSession(ip, token), 'closed', {
          timeoutMs: 10000, onSession: applySession, waitUntilActive: waitUntilAppActive,
        });
      } else if (current.state === 'opening') {
        const opened = await pollPairingSession(() => getPairingSession(ip, token), 'active', {
          timeoutMs: 10000, onSession: applySession, waitUntilActive: waitUntilAppActive,
        });
        if (opened.kind !== 'ready') {
          setNotice('L’ouverture n’est pas confirmée; la session reste affichée pour vérification.');
          return;
        }
        result = await closePairingSessionOnce(() => closePairingSession(ip, token),
          () => getPairingSession(ip, token), {
            timeoutMs: 10000, onSession: applySession, waitUntilActive: waitUntilAppActive,
          });
      } else {
        result = await closePairingSessionOnce(() => closePairingSession(ip, token),
          () => getPairingSession(ip, token), {
            timeoutMs: 10000, onSession: applySession, waitUntilActive: waitUntilAppActive,
          });
      }
      if (result.kind === 'ready' || result.kind === 'none') {
        setSessionUncertain(false);
        onClose();
      } else if (result.kind === 'terminal') {
        setNotice(`Fermeture non confirmée : session « ${result.value?.state ?? 'inconnue'} ». Relisez son état avant de quitter.`);
      } else if (result.kind === 'timeout') {
        setNotice('Fermeture demandée mais non confirmée. Actualisez l’état; aucune nouvelle session ne sera ouverte automatiquement.');
      } else if (result.kind === 'unavailable') {
        setError(describeApiError(result.error));
      }
    } catch (failure) {
      setError(describeApiError(failure));
      void refreshState(true);
    } finally {
      setRefreshing(false);
      releaseAction();
    }
  }

  const selectedCandidate = findPairingCandidate(candidates, selectedHardwareId);
  const canStartPairing = session?.state === 'active' && selectedCandidate !== null &&
    isValidPairingName(name.trim()) && operationPhase !== 'uncertain' &&
    operation?.state !== 'uncertain' && operation?.state !== 'expired';
  const step = operation?.state === 'succeeded' && !selectedCandidate ? 5 : operationPhase === 'submitting' ||
    operationPhase === 'tracking' ? 4 : confirming ? 4 : selectedCandidate ? 3 :
      session?.state === 'active' ? 2 : 1;

  return (
    <Modal visible animationType="slide" onRequestClose={() => void requestClose()}>
      <SafeAreaView style={styles.root}>
        <ScrollView contentContainerStyle={styles.content} keyboardShouldPersistTaps="handled">
          <View style={styles.header}>
            <View>
              <Text style={styles.eyebrow}>SMART LIGHTING · ÉCLAIRAGE</Text>
              <Text style={styles.title}>Ajouter une lampe</Text>
            </View>
            <Pressable onPress={() => void requestClose()} accessibilityRole="button"
              accessibilityLabel="Fermer la session d’ajout" style={styles.closeButton}>
              <Text style={styles.closeText}>Terminer</Text>
            </Pressable>
          </View>

          <View style={styles.steps}>
            {['Rechercher', 'Sélectionner', 'Nommer', 'Appairer', 'Terminer'].map((label, index) => (
              <View key={label} style={[styles.step, step >= index + 1 && styles.stepActive]}>
                <Text style={[styles.stepText, step >= index + 1 && styles.stepTextActive]}>
                  {index + 1} · {label}
                </Text>
              </View>
            ))}
          </View>

          <View style={styles.coreCard}>
            <View style={styles.coreTop}>
              <Text style={styles.coreLabel}>CORE-WIFI · {ip}</Text>
              <StatusIndicator label={reachable === true ? 'Joignable' : reachable === false ? 'Inaccessible' : 'Vérification'}
                tone={reachable === true ? 'good' : reachable === false ? 'bad' : 'neutral'} onDark />
            </View>
            <Text style={styles.sessionState}>{sessionLabel(session?.state)}</Text>
            {sessionOrigin === 'existing' && session?.state === 'active' ?
              <Text style={styles.coreNote}>Session existante reprise; aucune nouvelle fenêtre radio n’a été ouverte.</Text> : null}
            {session?.state === 'active' && remainingSeconds !== null ?
              <Text style={styles.coreNote}>{remainingSeconds > 0 ?
                `Expiration estimée dans ${remainingSeconds} s` :
                'Durée estimée écoulée; la fermeture doit encore être confirmée par le CORE.'}</Text> : null}
          </View>

          {loading ? <ActivityIndicator size="large" color="#18785d" style={styles.loader} /> : null}
          {error ? <Text style={styles.error}>{error}</Text> : null}
          {notice ? <Text style={styles.notice}>{notice}</Text> : null}
          {!loading && session?.state !== 'active' ?
            <Pressable style={styles.stateRefresh} onPress={() => void refreshState(true)}
              disabled={refreshing} accessibilityRole="button">
              <Text style={styles.link}>{refreshing ? 'Lecture de la session…' : 'Vérifier l’état de la session'}</Text>
            </Pressable> : null}

          {!loading && !session && reachable === true ? <View style={styles.card}>
            <Text style={styles.sectionTitle}>Prêt à rechercher</Text>
            <Text style={styles.body}>La recherche radio ne démarre que lorsque vous touchez ce bouton. Elle dure 60 secondes.</Text>
            <Pressable style={[styles.primary, (refreshing || sessionUncertain) && styles.disabled]}
              onPress={() => void startSearch()} disabled={refreshing || sessionUncertain}
              accessibilityRole="button">
              {refreshing ? <ActivityIndicator color="#fff" /> :
                <Text style={styles.primaryText}>Démarrer la recherche</Text>}
            </Pressable>
          </View> : null}

          {session?.state === 'opening' ? <View style={styles.card}>
            <ActivityIndicator color="#18785d" />
            <Text style={styles.body}>Le MAIN doit confirmer l’ouverture. Les candidates seront lues après cet ACK.</Text>
          </View> : null}

          {session?.state === 'active' ? <View style={styles.card}>
            <View style={styles.sectionHeader}>
              <View>
                <Text style={styles.sectionTitle}>Lampes détectées</Text>
                <Text style={styles.body}>{candidates.length} candidate{candidates.length === 1 ? '' : 's'} · maximum 10</Text>
              </View>
              <Pressable disabled={refreshing} onPress={() => void refreshState(true)}
                accessibilityRole="button"><Text style={styles.link}>{refreshing ? 'Lecture…' : 'Actualiser'}</Text></Pressable>
            </View>
            {capacityReached ? <Text style={styles.warning}>Capacité maximale de candidates atteinte.</Text> : null}
            {candidates.length === 0 ? <Text style={styles.body}>Aucune lampe non appairée détectée pour le moment. Laissez-la alimentée puis actualisez.</Text> : null}
            {candidates.map((candidate) => {
              const selected = candidate.hardware_id === selectedHardwareId;
              return <Pressable key={candidate.hardware_id}
                style={[styles.candidate, selected && styles.candidateSelected]}
                onPress={() => {
                  if (actionLock.current.active || operationPhase === 'uncertain' ||
                      operation?.state === 'uncertain' || operation?.state === 'expired') return;
                  setSelectedHardwareId(candidate.hardware_id);
                  selectedRef.current = candidate.hardware_id;
                  setName('');
                  setConfirming(false);
                  setNotice('Candidate sélectionnée par son identifiant matériel.');
                }} accessibilityRole="button" accessibilityState={{ selected }}>
                <Text style={styles.candidateTitle}>{candidate.hardware_id}</Text>
                <Text style={styles.candidateInfo}>Capacités : 0x{candidate.capabilities.toString(16).toUpperCase()}</Text>
                <Text style={styles.candidateInfo}>Dernière annonce : {candidate.last_seen_ms} ms (temps firmware)</Text>
                <Text style={styles.selectText}>{selected ? 'Sélectionnée' : 'Sélectionner cette lampe'}</Text>
              </Pressable>;
            })}
          </View> : null}

          {selectedCandidate && session?.state === 'active' && !operationIsTerminal(operation?.state) ?
            <View style={styles.card}>
              <Text style={styles.sectionTitle}>Nommer la lampe</Text>
              <Text style={styles.body}>Identité matérielle : {selectedCandidate.hardware_id}</Text>
              <TextInput style={styles.input} value={name} onChangeText={(value) => {
                setName(value);
                setConfirming(false);
              }} placeholder="Lampe salon" maxLength={31} autoCapitalize="none"
                autoCorrect={false} accessibilityLabel="Nom de la nouvelle lampe" />
              <Text style={styles.hint}>1 à 31 caractères ASCII : lettres, chiffres, espace, _, -, . ou apostrophe.</Text>
              {isValidPairingName(name.trim()) && !confirming ?
                <Pressable style={styles.secondary} onPress={() => setConfirming(true)}
                  accessibilityRole="button"><Text style={styles.secondaryText}>Continuer</Text></Pressable> : null}
              {confirming ? <View style={styles.confirmBox}>
                <Text style={styles.confirmTitle}>Confirmer l’appairage</Text>
                <Text style={styles.body}>Ajouter « {name.trim()} » ({selectedCandidate.hardware_id}) à Smart Lighting ?</Text>
                <Pressable style={[styles.primary, (actionBusy || !canStartPairing) && styles.disabled]}
                  onPress={() => void confirmPairing()} disabled={!canStartPairing || actionBusy}
                  accessibilityRole="button"><Text style={styles.primaryText}>Confirmer et appairer</Text></Pressable>
                <Pressable style={styles.textButton} onPress={() => setConfirming(false)}
                  accessibilityRole="button"><Text style={styles.link}>Annuler</Text></Pressable>
              </View> : null}
            </View> : null}

          {operation ? <View style={styles.card}>
            <Text style={styles.sectionTitle}>{operationLabel(operation.state)}</Text>
            <Text style={styles.body}>Opération : {operation.id}</Text>
            <Text style={styles.body}>Lampe : {operation.name ?? name.trim()} · {operation.hardware_id}</Text>
            {operationResultMessage(operation.result_code) ?
              <Text style={styles.warning}>{operationResultMessage(operation.result_code)}</Text> : null}
            {operation.result_code ? <Text style={styles.body}>Code : {operation.result_code}</Text> : null}
            {operation.local_device_id ? <Text style={styles.body}>ID local attribué : {operation.local_device_id}</Text> : null}
            {operationError ? <Text style={styles.warning}>{operationError}</Text> : null}
            {operation.state === 'succeeded' && inventoryMessage ?
              <Text style={styles.success}>{inventoryMessage}</Text> : null}
            {operationPhase === 'tracking' || operationPhase === 'submitting' ?
              <ActivityIndicator color="#18785d" style={styles.smallLoader} /> : null}
            {(operationPhase === 'uncertain' || operation.state === 'uncertain') ?
              <Pressable style={styles.secondary} onPress={() => void checkOperationAgain()}
                disabled={actionBusy} accessibilityRole="button">
                <Text style={styles.secondaryText}>Vérifier l’opération</Text>
              </Pressable> : null}
            {operation.state === 'failed' || operation.state === 'rejected' ?
              <Text style={styles.warning}>Aucun nouvel envoi n’a été effectué. Corrigez le problème avant de lancer une nouvelle demande.</Text> : null}
          </View> : null}

          {session?.state === 'uncertain' ? <View style={styles.card}>
            <Text style={styles.warning}>La session radio est incertaine. Demandez sa fermeture ou actualisez son état; aucune autre recherche ne sera ouverte automatiquement.</Text>
          </View> : null}
          {session?.state === 'expired' || session?.state === 'closed' || session?.state === 'failed' ?
            <View style={styles.card}><Text style={styles.body}>La session est terminée. Vous pouvez démarrer explicitement une nouvelle recherche.</Text>
              <Pressable style={styles.primary} onPress={() => { applySession(null); setSessionOrigin(null); setSessionUncertain(false); }}
                accessibilityRole="button"><Text style={styles.primaryText}>Revenir à l’ajout</Text></Pressable>
            </View> : null}

          <View style={styles.footerCard}>
            <Text style={styles.footerText}>Une candidate n’est pas encore une lampe active. L’appairage est confirmé uniquement par l’état final de l’API; la lampe apparaît ensuite dans l’inventaire.</Text>
            <Text style={styles.footerText}>La recherche est limitée au CORE-WIFI configuré. Fermez la session avant de modifier cette connexion.</Text>
          </View>
        </ScrollView>
      </SafeAreaView>
    </Modal>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#f4f7f4' },
  content: { padding: 20, paddingBottom: 40 },
  header: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginBottom: 20 },
  eyebrow: { color: '#17805f', fontSize: 10, fontWeight: '800', letterSpacing: 1.4 },
  title: { color: '#172b36', fontSize: 27, fontWeight: '800', marginTop: 5 },
  closeButton: { borderWidth: 1, borderColor: '#dde6e0', backgroundColor: '#fff', borderRadius: 12, paddingHorizontal: 13, paddingVertical: 10 },
  closeText: { color: '#476256', fontSize: 12, fontWeight: '700' },
  steps: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginBottom: 16 },
  step: { borderRadius: 10, backgroundColor: '#e8eeea', paddingHorizontal: 9, paddingVertical: 7 },
  stepActive: { backgroundColor: '#d5eee2' },
  stepText: { color: '#82929a', fontSize: 10, fontWeight: '700' },
  stepTextActive: { color: '#18785d' },
  coreCard: { backgroundColor: '#183c35', borderRadius: 20, padding: 18, marginBottom: 14 },
  coreTop: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 8 },
  coreLabel: { color: '#c1d7cf', fontSize: 11, fontWeight: '700', flexShrink: 1 },
  sessionState: { color: '#fff', fontSize: 20, fontWeight: '800', marginTop: 14 },
  coreNote: { color: '#c1d7cf', fontSize: 12, lineHeight: 18, marginTop: 7 },
  card: { backgroundColor: '#fff', borderRadius: 18, borderWidth: 1, borderColor: '#e4ebe6', padding: 18, marginBottom: 13 },
  sectionHeader: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 10, marginBottom: 12 },
  sectionTitle: { color: '#172b36', fontSize: 17, fontWeight: '800' },
  body: { color: '#61747b', fontSize: 13, lineHeight: 19, marginTop: 7 },
  primary: { minHeight: 47, alignItems: 'center', justifyContent: 'center', backgroundColor: '#18785d', borderRadius: 13, paddingHorizontal: 15, marginTop: 16 },
  primaryText: { color: '#fff', fontSize: 14, fontWeight: '800' },
  secondary: { minHeight: 44, alignItems: 'center', justifyContent: 'center', backgroundColor: '#edf5f0', borderRadius: 12, paddingHorizontal: 14, marginTop: 13 },
  secondaryText: { color: '#18785d', fontSize: 13, fontWeight: '800' },
  disabled: { opacity: 0.5 },
  link: { color: '#18785d', fontSize: 13, fontWeight: '800', padding: 7 },
  stateRefresh: { alignItems: 'center', marginBottom: 12 },
  candidate: { borderColor: '#e0e8e3', borderWidth: 1, borderRadius: 13, padding: 14, marginTop: 9 },
  candidateSelected: { borderColor: '#18785d', backgroundColor: '#f2faf6', borderWidth: 2 },
  candidateTitle: { color: '#203942', fontSize: 14, fontWeight: '800' },
  candidateInfo: { color: '#71828a', fontSize: 11, marginTop: 5 },
  selectText: { color: '#18785d', fontSize: 12, fontWeight: '800', marginTop: 10 },
  input: { borderWidth: 1, borderColor: '#dce5df', borderRadius: 12, paddingHorizontal: 13, paddingVertical: 12, color: '#172b36', fontSize: 15, marginTop: 12 },
  hint: { color: '#87969c', fontSize: 11, lineHeight: 16, marginTop: 7 },
  confirmBox: { backgroundColor: '#f3f8f5', borderRadius: 14, padding: 14, marginTop: 15 },
  confirmTitle: { color: '#203942', fontSize: 15, fontWeight: '800' },
  textButton: { alignItems: 'center', marginTop: 5 },
  warning: { color: '#875a16', backgroundColor: '#fff5df', padding: 11, borderRadius: 10, fontSize: 12, lineHeight: 18, marginTop: 10 },
  success: { color: '#176344', backgroundColor: '#e6f6ed', padding: 11, borderRadius: 10, fontSize: 12, lineHeight: 18, marginTop: 10 },
  error: { color: '#9d3e3a', backgroundColor: '#fff0ef', padding: 12, borderRadius: 11, fontSize: 12, lineHeight: 18, marginBottom: 12 },
  notice: { color: '#476256', backgroundColor: '#eaf4ee', padding: 12, borderRadius: 11, fontSize: 12, lineHeight: 18, marginBottom: 12 },
  loader: { marginVertical: 24 },
  smallLoader: { marginTop: 13 },
  footerCard: { paddingHorizontal: 8, paddingVertical: 12 },
  footerText: { color: '#82929a', fontSize: 11, lineHeight: 17, textAlign: 'center', marginTop: 5 },
});
