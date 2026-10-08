import { useCallback, useEffect, useRef, useState } from 'react';
import { ActivityIndicator, AppState, Modal, Pressable, RefreshControl,
  ScrollView, StyleSheet, Text, View } from 'react-native';
import { describeApiError, getDevice, getDevices, getHealth, getModules,
  lampsOnly } from '../../api/coreApiClient';
import type { HealthResponse, LampRecord, ModuleRecord, PowerState, V7Id } from '../../api/types';
import { StatusIndicator } from '../../components/StatusIndicator';
import { LampCard } from './LampCard';
import type { LampCommandFeedback } from './commandPolling';

const POLL_INTERVAL_MS = 20000;

interface Snapshot {
  health: HealthResponse;
  modules: ModuleRecord[];
  lamps: LampRecord[];
  receivedAt: number;
}

export function LampListScreen({ ip, onSettings, commandFeedback, onPower }: {
  ip: string;
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
  const requestVersion = useRef(0);
  const detailVersion = useRef(0);
  const screenMounted = useRef(true);

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
    };
  }, [refresh]);

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

  const old = Boolean(error) || revalidating;
  const onlineModules = snapshot?.modules.filter((module) => module.online).length ?? 0;

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
          onPower={(state) => void requestPower(lamp.id, state)}
          onPress={() => void openDetail(lamp.id)} />)}
        {snapshot ? <Text style={styles.updated}>Dernière lecture · {new Date(snapshot.receivedAt).toLocaleTimeString()}</Text> : null}
        <Text style={styles.footer}>L’état confirmé est mis à jour après ACK et relecture du CORE. Aucune mesure électrique réelle n’est disponible.</Text>
      </ScrollView>

      <Modal visible={selectedId !== null} animationType="slide" transparent
        onRequestClose={closeDetail}>
        <View style={styles.modalBackdrop}>
          <View style={styles.modalCard}>
            <View style={styles.modalHeader}>
              <Text style={styles.modalTitle}>Détail de la lampe</Text>
              <Pressable onPress={closeDetail} accessibilityRole="button"
                accessibilityLabel="Fermer le détail"><Text style={styles.close}>Fermer</Text></Pressable>
            </View>
            {detailLoading ? <ActivityIndicator color="#18785d" style={styles.loader} /> : null}
            {detailError ? <Text style={styles.errorText}>{detailError}</Text> : null}
            {detail ? <>
              <Text style={styles.detailName}>{detail.name}</Text>
              <Text style={styles.detailLine}>ID V7 : {detail.id}</Text>
              <Text style={styles.detailLine}>MAIN parent : {detail.parent_id}</Text>
              <Text style={styles.detailLine}>Disponibilité : {detail.online ? 'en ligne' : 'hors ligne'}</Text>
              <Text style={styles.detailLine}>Dernier état confirmé : {detail.last_confirmed_state.power?.toUpperCase() ?? 'inconnu'}</Text>
              <Text style={styles.detailLine}>Statut : {detail.last_confirmed_state.status}</Text>
              <Text style={styles.detailNote}>Historique logiciel reçu du CORE. La lampe ne fournit pas de mesure électrique.</Text>
            </> : null}
          </View>
        </View>
      </Modal>
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
    padding: 24, paddingBottom: 42 },
  modalHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  modalTitle: { color: '#172b36', fontSize: 18, fontWeight: '800' },
  close: { color: '#18785d', fontSize: 13, fontWeight: '700' },
  detailName: { color: '#172b36', fontSize: 24, fontWeight: '800', marginTop: 24, marginBottom: 10 },
  detailLine: { color: '#4a626c', fontSize: 14, marginTop: 8 },
  detailNote: { color: '#82929a', fontSize: 12, lineHeight: 18, marginTop: 22 },
  errorText: { color: '#b64e49', fontSize: 13, marginTop: 20 },
});
