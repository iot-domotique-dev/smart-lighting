import { useCallback, useEffect, useRef, useState } from 'react';
import * as Crypto from 'expo-crypto';
import { ActivityIndicator, Alert, Pressable, RefreshControl, ScrollView,
  StyleSheet, Text, TextInput, View } from 'react-native';
import { CoreApiError, changeGroupMember, createGroup, deleteGroup,
  describeApiError, getGroupOperation, getGroups, isValidGroupName, renameGroup,
  setGroupPower }
  from '../../api/coreApiClient';
import type { GroupOperation, GroupRecord, LampRecord, V7Id } from '../../api/types';
import { groupMemberResultLabel, groupOperationResultLabel,
  groupOperationStateLabel } from './groupOperationLabels';
import { clearPendingGroupPowerOperation, loadPendingGroupPowerOperation,
  savePendingGroupPowerOperation } from './groupOperationStore';

const OPERATION_POLL_MS = 500;
const OPERATION_MAX_POLLS = 44;

function makeOperationId(): Promise<V7Id> {
  return Crypto.getRandomBytesAsync(4).then((bytes) => {
    const id = (bytes[0] * 0x1000000 + bytes[1] * 0x10000 +
      bytes[2] * 0x100 + bytes[3]) >>> 0;
    return id === 0 ? 1 : id;
  });
}

function isTerminal(operation: GroupOperation): boolean {
  return operation.state === 'persisted' || operation.state === 'completed' ||
    operation.state === 'failed' || operation.state === 'uncertain';
}

function wait(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function invalidateOperationVersion(version: { current: number }): void {
  version.current++;
}

export function GroupsScreen({ ip, token, mainId, mainOnline, lamps, refreshSignal, active,
  onClose }: {
  ip: string;
  token: string;
  mainId: V7Id;
  mainOnline: boolean;
  lamps: LampRecord[];
  refreshSignal: number;
  active: boolean;
  onClose: () => void;
}) {
  const [groups, setGroups] = useState<GroupRecord[] | null>(null);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState('');
  const [message, setMessage] = useState('');
  const [name, setName] = useState('');
  const [renameValues, setRenameValues] = useState<Record<string, string>>({});
  const [busy, setBusy] = useState(false);
  const [pending, setPending] = useState<GroupOperation | null>(null);
  const [uncertain, setUncertain] = useState(false);
  const operationLock = useRef(false);
  const runVersion = useRef(0);
  const mounted = useRef(true);

  const refresh = useCallback(async (manual = false) => {
    if (manual) setRefreshing(true);
    try {
      const result = await getGroups(ip, mainId, token);
      if (!mounted.current) return;
      setGroups(result.groups);
      setError('');
    } catch (failure) {
      if (mounted.current) setError(describeApiError(failure));
    } finally {
      if (mounted.current) {
        setLoading(false);
        setRefreshing(false);
      }
    }
  }, [ip, mainId, token]);

  const trackOperation = useCallback(async (operationId: V7Id,
    initial?: GroupOperation) => {
    const version = ++runVersion.current;
    let current = initial;
    if (current) setPending(current);
    for (let attempt = 0; attempt < OPERATION_MAX_POLLS; attempt++) {
      if (!mounted.current || version !== runVersion.current) return;
      if (current && isTerminal(current)) break;
      if (attempt !== 0) await wait(OPERATION_POLL_MS);
      try {
        const response = await getGroupOperation(ip, operationId, token);
        current = response.operation;
        if (mounted.current && version === runVersion.current) setPending(current);
      } catch (failure) {
        if (failure instanceof CoreApiError && failure.kind === 'http' &&
            failure.code === 'group_operation_not_found') {
          setUncertain(true);
          setMessage('Le CORE ne conserve plus le suivi. La modification peut avoir été appliquée; aucun nouveau POST ne sera envoyé. Relisez les groupes.');
          await refresh();
          return;
        }
        // A failed status GET cannot prove the mutation did not happen.
        if (mounted.current && version === runVersion.current) {
          setUncertain(true);
          setMessage(`Suivi interrompu : ${describeApiError(failure)} Aucun nouveau POST ne sera envoyé.`);
        }
        return;
      }
    }
    if (!current || !isTerminal(current)) {
      setUncertain(true);
      setMessage('Le résultat n’est pas confirmé. Relisez les groupes avant toute autre modification; la demande ne sera pas renvoyée.');
      await refresh();
      return;
    }
    if (current.state !== 'uncertain') {
      try {
        const saved = await loadPendingGroupPowerOperation();
        if (saved?.operationId === operationId) await clearPendingGroupPowerOperation();
      } catch {
        if (mounted.current) {
          setUncertain(true);
          setMessage('Le résultat est terminal, mais le suivi local n’a pas pu être effacé. Ne renvoyez pas la commande avant d’avoir relu son état.');
        }
      }
    }
    if (current.state === 'persisted') {
      setUncertain(false);
      setMessage('Configuration sauvegardée sur MAIN.');
      await refresh();
    } else if (current.state === 'completed' && current.result === 'completed') {
      setUncertain(false);
      setMessage('Toutes les lampes ont confirmé la commande.');
    } else if (current.state === 'failed') {
      setUncertain(false);
      setMessage(current.result === 'revision_conflict'
        ? 'Le groupe a changé depuis son affichage. La configuration vient d’être relue.'
        : `MAIN a refusé l’opération (${current.result ?? 'failed'}).`);
      await refresh();
    } else if (current.state === 'completed' && current.result === 'partial') {
      setUncertain(false);
      setMessage('Commande terminée avec des résultats différents selon les lampes.');
    } else {
      setUncertain(true);
      setMessage('L’opération reste incertaine. Aucun nouveau POST ne sera envoyé; relisez la configuration sur MAIN.');
      await refresh();
    }
  }, [ip, refresh, token]);

  useEffect(() => {
    if (!active) return undefined;
    mounted.current = true;
    const scheduledRefresh = setTimeout(() => void refresh(), 0);
    const scheduledRecovery = setTimeout(() => {
      void (async () => {
        let saved;
        try {
          saved = await loadPendingGroupPowerOperation();
        } catch (failure) {
          if (mounted.current) {
            setUncertain(true);
            setMessage(failure instanceof Error ? failure.message :
              'Le suivi local est illisible. Ne renvoyez pas la commande.');
          }
          return;
        }
        if (!saved || !mounted.current) return;
        setPending({ id: saved.operationId, state: 'sent', group_id: saved.groupId });
        operationLock.current = true;
        setBusy(true);
        if (saved.coreIp !== ip || saved.mainId !== mainId) {
          setUncertain(true);
          setMessage('Une commande de groupe reste à vérifier sur une autre connexion CORE. Aucun nouveau POST ne sera envoyé.');
          operationLock.current = false;
          setBusy(false);
          return;
        }
        setMessage('Reprise du suivi de la commande collective…');
        try {
          const current = await getGroupOperation(ip, saved.operationId, token);
          if (mounted.current) await trackOperation(saved.operationId, current.operation);
        } catch (failure) {
          if (mounted.current) {
            setUncertain(true);
            setMessage(`Le suivi de la commande ${saved.operationId} est introuvable ou inaccessible : ${describeApiError(failure)} Aucun nouveau POST ne sera envoyé.`);
          }
        } finally {
          operationLock.current = false;
          if (mounted.current) setBusy(false);
        }
      })();
    }, 0);
    return () => {
      mounted.current = false;
      invalidateOperationVersion(runVersion);
      clearTimeout(scheduledRefresh);
      clearTimeout(scheduledRecovery);
    };
  }, [active, ip, mainId, refresh, token, trackOperation]);

  useEffect(() => {
    if (!active || refreshSignal === 0) return undefined;
    const scheduledRefresh = setTimeout(() => void refresh(), 0);
    return () => clearTimeout(scheduledRefresh);
  }, [active, refreshSignal, refresh]);

  const submitGroupPower = useCallback(async (groupId: V7Id,
    power: 'on' | 'off') => {
    if (operationLock.current || busy || uncertain) return;
    operationLock.current = true;
    setBusy(true);
    setError('');
    setMessage('Préparation de la commande collective…');
    setPending(null);
    let operationId: V7Id | undefined;
    let postAttempted = false;
    try {
      operationId = await makeOperationId();
      await savePendingGroupPowerOperation({ operationId, mainId, groupId, power, coreIp: ip });
      postAttempted = true;
      const response = await setGroupPower(ip, mainId, groupId,
        { operation_id: operationId, power }, token);
      setPending(response.operation);
      await trackOperation(operationId, response.operation);
    } catch (failure) {
      if (failure instanceof CoreApiError && failure.kind === 'submission_uncertain' &&
          operationId !== undefined) {
        setPending({ id: operationId, state: 'uncertain', group_id: groupId });
        setMessage('La réponse HTTP a été perdue. Vérification du même identifiant; aucun nouveau POST ne sera envoyé.');
        setUncertain(true);
        try {
          const current = await getGroupOperation(ip, operationId, token);
          setUncertain(false);
          await trackOperation(operationId, current.operation);
        } catch {
          await refresh();
          setMessage(`Le suivi de la commande ${operationId} est introuvable ou inaccessible. Elle a peut-être été exécutée; aucun nouveau POST ne sera envoyé.`);
        }
      } else {
        if (failure instanceof CoreApiError && failure.kind === 'http' &&
            failure.status !== undefined && failure.status >= 400 && failure.status < 500 &&
            operationId !== undefined) {
          try {
            await clearPendingGroupPowerOperation();
          } catch {
            setPending({ id: operationId, state: 'uncertain', group_id: groupId });
            setUncertain(true);
            setMessage('Le CORE a refusé la demande, mais le suivi local n’a pas pu être effacé. Vérifiez avant toute nouvelle commande.');
            return;
          }
        } else if (postAttempted && operationId !== undefined) {
          setPending({ id: operationId, state: 'uncertain', group_id: groupId });
          setUncertain(true);
          setMessage(`Résultat d’envoi incertain : ${describeApiError(failure)} Aucun nouveau POST ne sera envoyé.`);
          return;
        } else {
          setError(describeApiError(failure));
          return;
        }
        setError(describeApiError(failure));
      }
    } finally {
      operationLock.current = false;
      setBusy(false);
    }
  }, [busy, ip, mainId, refresh, token, trackOperation, uncertain]);

  const forgetUncertainGroupOperation = useCallback(async () => {
    try {
      await clearPendingGroupPowerOperation();
      setPending(null);
      setUncertain(false);
      setMessage('Suivi local effacé. Vérifiez l’état des lampes avant d’envoyer une nouvelle commande.');
    } catch {
      setError('Impossible d’effacer le suivi local. Aucune nouvelle commande ne sera envoyée.');
    }
  }, []);

  const submit = useCallback(async (send: (operationId: V7Id) => Promise<{
    operation: GroupOperation;
  }>) => {
    if (operationLock.current || busy || uncertain) return;
    operationLock.current = true;
    setBusy(true);
    setError('');
    setMessage('Envoi vers MAIN…');
    setPending(null);
    let operationId: V7Id | undefined;
    try {
      operationId = await makeOperationId();
      const response = await send(operationId);
      setPending(response.operation);
      await trackOperation(operationId, response.operation);
    } catch (failure) {
      if (failure instanceof CoreApiError && failure.kind === 'submission_uncertain' &&
          operationId !== undefined) {
        setPending({ id: operationId, state: 'uncertain' });
        setMessage('La réponse du POST a été perdue. Vérification du même identifiant; aucune nouvelle demande ne sera envoyée.');
        setUncertain(true);
        // Query the known id once, without resending the mutation.
        try {
          const current = await getGroupOperation(ip, operationId, token);
          setUncertain(false);
          await trackOperation(operationId, current.operation);
        } catch {
          await refresh();
          setMessage('Le suivi du POST est introuvable ou inaccessible. La modification peut avoir été appliquée; aucun nouveau POST ne sera envoyé.');
        }
      } else {
        setError(describeApiError(failure));
        if (failure instanceof CoreApiError && failure.code === 'revision_conflict')
          await refresh();
      }
    } finally {
      operationLock.current = false;
      setBusy(false);
    }
  }, [busy, ip, refresh, token, trackOperation, uncertain]);

  async function requestCreate() {
    if (!isValidGroupName(name)) {
      setError('Choisissez un nom de 1 à 31 caractères imprimables.');
      return;
    }
    const newName = name.trim();
    await submit((operationId) => createGroup(ip, mainId,
      { operation_id: operationId, name: newName }, token));
    setName('');
  }

  function requestDelete(group: GroupRecord) {
    Alert.alert('Supprimer ce groupe ?',
      `« ${group.name} » et sa configuration seront supprimés sur MAIN.`, [
        { text: 'Annuler', style: 'cancel' },
        { text: 'Supprimer', style: 'destructive', onPress: () => {
          void submit((operationId) => deleteGroup(ip, mainId, group.id, {
            operation_id: operationId, expected_revision: group.revision,
          }, token));
        } },
      ]);
  }

  const lampById = new Map(lamps.filter((lamp) => lamp.parent_id === mainId)
    .map((lamp) => [lamp.id, lamp]));

  return (
    <View style={styles.root}>
      <View style={styles.header}>
        <View>
          <Text style={styles.eyebrow}>MAIN · CONFIGURATION</Text>
          <Text style={styles.title}>Groupes</Text>
        </View>
        <Pressable onPress={onClose} disabled={busy}
          accessibilityRole="button" accessibilityLabel="Fermer les groupes">
          <Text style={styles.close}>Fermer</Text>
        </Pressable>
      </View>
      <Text style={styles.subtitle}>Les groupes et leurs membres sont enregistrés sur MAIN.</Text>
      {!mainOnline ? <Text style={styles.warning}>MAIN est hors ligne. La configuration ne peut pas être lue ou modifiée.</Text> : null}
      <ScrollView contentContainerStyle={styles.content}
        refreshControl={<RefreshControl refreshing={refreshing}
          onRefresh={() => void refresh(true)} tintColor="#18785d" />}>
        <View style={styles.createCard}>
          <Text style={styles.sectionTitle}>Créer un groupe</Text>
          <TextInput value={name} onChangeText={setName} maxLength={31}
            placeholder="Ex. Salon" placeholderTextColor="#8b999e"
            style={styles.input} editable={!busy && !uncertain}
            accessibilityLabel="Nom du nouveau groupe" />
          <Pressable onPress={() => void requestCreate()}
            disabled={!mainOnline || busy || uncertain}
            style={[styles.primaryButton, (!mainOnline || busy || uncertain) && styles.dim]}>
            <Text style={styles.primaryText}>Créer sur MAIN</Text>
          </Pressable>
        </View>
        {loading && !groups ? <ActivityIndicator color="#18785d" style={styles.loader} /> : null}
        {error ? <View style={styles.errorCard}>
          <Text style={styles.errorText}>{error}</Text>
          <Pressable onPress={() => void refresh(true)}><Text style={styles.actionText}>Relire les groupes</Text></Pressable>
        </View> : null}
        {message ? <View style={[styles.messageCard, uncertain && styles.warningCard]}>
          <Text style={styles.messageText}>{message}</Text>
          {pending ? <Text style={styles.operationText}>Opération {pending.id} · {groupOperationStateLabel(pending.state)}{pending.result ? ` · ${groupOperationResultLabel(pending.result)}` : ''}</Text> : null}
          {uncertain ? <Pressable onPress={() => void refresh(true)}>
            <Text style={styles.actionText}>Relire la configuration</Text>
          </Pressable> : null}
          {uncertain && pending?.id ? <Pressable onPress={() => void forgetUncertainGroupOperation()}>
            <Text style={styles.actionText}>Effacer le suivi local après vérification</Text>
          </Pressable> : null}
        </View> : null}
        {groups?.length === 0 ? <View style={styles.empty}>
          <Text style={styles.emptyTitle}>Aucun groupe</Text>
          <Text style={styles.emptyText}>Créez un groupe puis choisissez les lampes associées à ce MAIN.</Text>
        </View> : null}
        {groups?.map((group) => {
          const renameValue = renameValues[String(group.id)] ?? group.name;
          const memberSet = new Set(group.member_ids);
          const groupCommandDisabled = !mainOnline || busy || uncertain ||
            group.member_ids.length === 0;
          return <View key={group.id} style={styles.groupCard}>
            <Text style={styles.groupTitle}>{group.name}</Text>
            <Text style={styles.meta}>ID {group.id} · révision {group.revision} · {group.member_ids.length} lampe(s)</Text>
            <View style={styles.powerRow}>
              <Pressable disabled={groupCommandDisabled}
                onPress={() => void submitGroupPower(group.id, 'on')}
                style={[styles.powerButton, styles.onButton,
                  groupCommandDisabled && styles.dim]}>
                <Text style={styles.powerText}>Allumer le groupe</Text>
              </Pressable>
              <Pressable disabled={groupCommandDisabled}
                onPress={() => void submitGroupPower(group.id, 'off')}
                style={[styles.powerButton, styles.offButton,
                  groupCommandDisabled && styles.dim]}>
                <Text style={styles.powerText}>Éteindre</Text>
              </Pressable>
            </View>
            <View style={styles.renameRow}>
              <TextInput value={renameValue} onChangeText={(value) =>
                setRenameValues((current) => ({ ...current, [String(group.id)]: value }))}
                maxLength={31} style={[styles.input, styles.renameInput]}
                editable={!busy && !uncertain} accessibilityLabel={`Nouveau nom pour ${group.name}`} />
              <Pressable disabled={!mainOnline || busy || uncertain || renameValue.trim() === group.name}
                onPress={() => {
                  const newName = renameValue.trim();
                  if (!isValidGroupName(newName)) { setError('Nom de groupe invalide.'); return; }
                  void submit((operationId) => renameGroup(ip, mainId, group.id, {
                    operation_id: operationId, expected_revision: group.revision, name: newName,
                  }, token));
                }}>
                <Text style={styles.actionText}>Renommer</Text>
              </Pressable>
            </View>
            <Text style={styles.memberTitle}>Membres</Text>
            {group.member_ids.map((lampId) => {
              const lamp = lampById.get(lampId);
              return <View key={lampId} style={styles.memberRow}>
                <Text style={styles.memberName}>{lamp?.name ?? `Lampe ${lampId}`}</Text>
                <Pressable disabled={!mainOnline || busy || uncertain} onPress={() => void submit((operationId) =>
                  changeGroupMember(ip, mainId, group.id, {
                    operation_id: operationId, expected_revision: group.revision,
                    member_action: 'remove', lamp_id: lampId,
                  }, token))}>
                  <Text style={styles.removeText}>Retirer</Text>
                </Pressable>
              </View>;
            })}
            {lamps.filter((lamp) => lamp.parent_id === mainId && !memberSet.has(lamp.id))
              .map((lamp) => <View key={lamp.id} style={styles.memberRow}>
                <Text style={styles.memberName}>{lamp.name}{lamp.online ? '' : ' · hors ligne'}</Text>
                <Pressable disabled={!mainOnline || busy || uncertain} onPress={() => void submit((operationId) =>
                  changeGroupMember(ip, mainId, group.id, {
                    operation_id: operationId, expected_revision: group.revision,
                    member_action: 'add', lamp_id: lamp.id,
                  }, token))}>
                  <Text style={styles.actionText}>Ajouter</Text>
                </Pressable>
              </View>)}
            <Pressable disabled={!mainOnline || busy || uncertain} onPress={() => requestDelete(group)}
              style={styles.deleteButton}>
              <Text style={styles.deleteText}>Supprimer le groupe</Text>
            </Pressable>
          </View>;
        })}
        <Pressable onPress={() => void refresh(true)} disabled={refreshing || busy}
          style={styles.refreshButton}>
          <Text style={styles.actionText}>Actualiser depuis MAIN</Text>
        </Pressable>
      </ScrollView>
      {pending?.members && pending.members.length > 0 ? <View style={styles.resultPanel}>
        <Text style={styles.resultTitle}>Résultat par lampe</Text>
        {pending.members.map((member) => <Text key={member.child_id} style={styles.resultItem}>
          {lampById.get(member.lamp_id)?.name ?? `Lampe ${member.lamp_id}`} · {groupMemberResultLabel(member)}
        </Text>)}
      </View> : null}
      {busy ? <View style={styles.busyBar}>
        <ActivityIndicator size="small" color="#18785d" />
        <Text style={styles.busyText}>Suivi de l’opération sur MAIN…</Text>
      </View> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#f4f7f4', paddingTop: 22 },
  header: { paddingHorizontal: 22, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  eyebrow: { color: '#17805f', fontSize: 10, fontWeight: '800', letterSpacing: 1.6 },
  title: { color: '#172b36', fontSize: 29, fontWeight: '800', marginTop: 4 },
  close: { color: '#18785d', fontSize: 13, fontWeight: '800', padding: 10 },
  subtitle: { color: '#6d7e84', fontSize: 12, paddingHorizontal: 22, marginTop: 8 },
  content: { padding: 22, paddingBottom: 42 },
  createCard: { backgroundColor: '#fff', borderRadius: 17, padding: 16, marginBottom: 16 },
  sectionTitle: { color: '#233940', fontSize: 16, fontWeight: '800' },
  input: { borderWidth: 1, borderColor: '#dce5df', backgroundColor: '#fff',
    borderRadius: 11, paddingHorizontal: 12, paddingVertical: 10, color: '#243a42', marginTop: 12 },
  primaryButton: { minHeight: 44, backgroundColor: '#18785d', borderRadius: 12,
    alignItems: 'center', justifyContent: 'center', marginTop: 10 },
  primaryText: { color: '#fff', fontSize: 13, fontWeight: '800' },
  dim: { opacity: 0.5 },
  groupCard: { backgroundColor: '#fff', borderRadius: 17, padding: 16, marginBottom: 14 },
  groupTitle: { color: '#203740', fontSize: 18, fontWeight: '800' },
  meta: { color: '#849398', fontSize: 11, marginTop: 4 },
  powerRow: { flexDirection: 'row', gap: 9, marginTop: 12 },
  powerButton: { flex: 1, minHeight: 42, borderRadius: 11,
    alignItems: 'center', justifyContent: 'center' },
  onButton: { backgroundColor: '#18785d' },
  offButton: { backgroundColor: '#4c6268' },
  powerText: { color: '#fff', fontSize: 12, fontWeight: '800' },
  renameRow: { flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 7 },
  renameInput: { flex: 1, marginTop: 0 },
  actionText: { color: '#18785d', fontSize: 12, fontWeight: '800', paddingVertical: 8 },
  memberTitle: { color: '#425b63', fontSize: 12, fontWeight: '800', marginTop: 14, marginBottom: 4 },
  memberRow: { minHeight: 39, flexDirection: 'row', justifyContent: 'space-between',
    alignItems: 'center', borderTopWidth: 1, borderTopColor: '#edf1ee' },
  memberName: { color: '#465d64', fontSize: 12, flex: 1 },
  removeText: { color: '#a64d42', fontSize: 12, fontWeight: '800', paddingVertical: 8 },
  deleteButton: { borderWidth: 1, borderColor: '#efd5d1', borderRadius: 11,
    alignItems: 'center', padding: 10, marginTop: 14 },
  deleteText: { color: '#a64d42', fontSize: 12, fontWeight: '800' },
  empty: { backgroundColor: '#fff', padding: 18, borderRadius: 15 },
  emptyTitle: { color: '#243a42', fontWeight: '800' },
  emptyText: { color: '#73838a', fontSize: 12, lineHeight: 18, marginTop: 7 },
  loader: { margin: 24 },
  errorCard: { padding: 14, backgroundColor: '#fff1ef', borderRadius: 13, marginBottom: 12 },
  errorText: { color: '#a7473f', fontSize: 12, lineHeight: 18 },
  messageCard: { padding: 14, backgroundColor: '#eaf4ee', borderRadius: 13, marginBottom: 12 },
  warningCard: { backgroundColor: '#fff2dc' },
  messageText: { color: '#345a48', fontSize: 12, lineHeight: 18 },
  operationText: { color: '#72817d', fontSize: 10, marginTop: 6 },
  resultPanel: { paddingHorizontal: 22, paddingVertical: 10,
    backgroundColor: '#f0f5f1', borderTopWidth: 1, borderTopColor: '#dce6df' },
  resultTitle: { color: '#34574a', fontSize: 11, fontWeight: '800', marginBottom: 4 },
  resultItem: { color: '#536d63', fontSize: 10, paddingVertical: 2 },
  warning: { color: '#855f20', backgroundColor: '#fff0d4', padding: 11,
    marginHorizontal: 22, marginTop: 12, borderRadius: 10, fontSize: 11 },
  refreshButton: { alignItems: 'center', padding: 10, marginTop: 2 },
  busyBar: { flexDirection: 'row', gap: 9, justifyContent: 'center',
    alignItems: 'center', padding: 12, backgroundColor: '#eaf4ee' },
  busyText: { color: '#45645a', fontSize: 11 },
});
