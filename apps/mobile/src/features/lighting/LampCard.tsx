import { Pressable, StyleSheet, Text, View } from 'react-native';
import type { LampRecord, PowerState } from '../../api/types';
import { StatusIndicator } from '../../components/StatusIndicator';
import { isCommandInProgress } from './commandPolling';
import type { LampCommandFeedback } from './commandPolling';

function feedbackText(feedback?: LampCommandFeedback): string | null {
  if (!feedback) return null;
  if (feedback.phase === 'submitting') return `Envoi de ${feedback.requested.toUpperCase()}…`;
  if (feedback.phase === 'tracking') {
    return `Commande ${feedback.commandState} · ${feedback.retries} reprise(s)`;
  }
  if (feedback.phase === 'refreshing') return feedback.message;
  if (feedback.phase === 'executed') {
    return `${feedback.requested.toUpperCase()} exécuté par ACK · ${feedback.retries} reprise(s)${
      feedback.refreshError ? ` · Relecture impossible : ${feedback.refreshError}` : ''}`;
  }
  return `${feedback.message}${feedback.refreshError ? ` · Relecture impossible : ${feedback.refreshError}` : ''}`;
}

export function LampCard({ lamp, mainOnline, snapshotOld, feedback, onPress, onPower }: {
  lamp: LampRecord;
  mainOnline: boolean;
  snapshotOld: boolean;
  feedback?: LampCommandFeedback;
  onPress: () => void;
  onPower: (state: PowerState) => void;
}) {
  const history = lamp.last_confirmed_state;
  const power = history.power === null ? 'Inconnu' : history.power.toUpperCase();
  const status = history.status === 'confirmed' ? 'Confirmé' :
    history.status === 'stale' ? 'À vérifier' : 'Inconnu';
  const busy = isCommandInProgress(feedback);
  const canControl = !busy && !snapshotOld && lamp.online && mainOnline;
  const message = feedbackText(feedback);
  const messageTone = feedback?.phase === 'failed' || feedback?.phase === 'rejected' ||
    feedback?.phase === 'uncertain' ? styles.commandError :
      feedback?.phase === 'executed' ? styles.commandSuccess : styles.commandInfo;

  return (
    <View style={styles.card}>
      <Pressable style={({ pressed }) => [styles.top, pressed && styles.pressed]}
        onPress={onPress} accessibilityRole="button"
        accessibilityLabel={`Détails de ${lamp.name}`}>
        <View style={styles.icon}><Text style={styles.iconText}>◉</Text></View>
        <View style={styles.identity}>
          <Text style={styles.name} numberOfLines={1}>{lamp.name}</Text>
          <Text style={styles.id}>ID V7 · {lamp.id}</Text>
        </View>
        <Text style={styles.chevron}>›</Text>
      </Pressable>
      <View style={styles.divider} />
      <View style={styles.bottom}>
        <View style={styles.stateBlock}>
          <Text style={styles.caption}>{history.status === 'stale' ?
            'DERNIER ÉTAT CONNU' : 'DERNIER ÉTAT CONFIRMÉ'}</Text>
          <Text style={styles.power}>{power}</Text>
          <Text style={styles.note}>Historique logiciel · état électrique non mesuré</Text>
        </View>
        <View style={styles.statuses}>
          <StatusIndicator label={snapshotOld ? 'Disponibilité inconnue' :
            lamp.online ? 'En ligne' : 'Hors ligne'}
            tone={snapshotOld ? 'neutral' : lamp.online ? 'good' : 'bad'} />
          <StatusIndicator label={snapshotOld && history.status === 'confirmed' ?
            'Non actualisé' : status}
            tone={snapshotOld ? 'warn' : history.status === 'confirmed' ? 'good' :
              history.status === 'stale' ? 'warn' : 'neutral'} />
          {!mainOnline ? <StatusIndicator label="MAIN indisponible" tone="bad" /> : null}
        </View>
      </View>

      {message ? <Text accessibilityLiveRegion="polite" style={[styles.commandMessage, messageTone]}>
        {message}
      </Text> : null}
      <View style={styles.controls}>
        <Pressable style={({ pressed }) => [styles.powerButton, styles.onButton,
          (!canControl || pressed) && styles.buttonDim]}
          onPress={() => onPower('on')} disabled={!canControl}
          accessibilityRole="button" accessibilityLabel={`Allumer ${lamp.name}`}>
          <Text style={styles.onText}>Allumer</Text>
        </Pressable>
        <Pressable style={({ pressed }) => [styles.powerButton, styles.offButton,
          (!canControl || pressed) && styles.buttonDim]}
          onPress={() => onPower('off')} disabled={!canControl}
          accessibilityRole="button" accessibilityLabel={`Éteindre ${lamp.name}`}>
          <Text style={styles.offText}>Éteindre</Text>
        </Pressable>
      </View>
      {!canControl && !busy ? <Text style={styles.disabledReason}>
        {snapshotOld ? 'Actualisez la disponibilité avant de commander.' :
          !lamp.online ? 'Lampe hors ligne.' : 'Vérifiez que son MAIN est en ligne.'}
      </Text> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  card: { backgroundColor: '#fff', borderRadius: 20, padding: 18,
    borderWidth: 1, borderColor: '#e6eae7', marginBottom: 12 },
  pressed: { opacity: 0.72 },
  top: { flexDirection: 'row', alignItems: 'center' },
  icon: { width: 42, height: 42, borderRadius: 14, backgroundColor: '#e7f4ec',
    justifyContent: 'center', alignItems: 'center', marginRight: 12 },
  iconText: { fontSize: 22, color: '#17805f' },
  identity: { flex: 1 },
  name: { color: '#172b36', fontSize: 17, fontWeight: '700' },
  id: { color: '#718095', fontSize: 12, marginTop: 3 },
  chevron: { color: '#8e9ba5', fontSize: 26 },
  divider: { height: 1, backgroundColor: '#edf0ef', marginVertical: 16 },
  bottom: { flexDirection: 'row', justifyContent: 'space-between', gap: 12 },
  stateBlock: { flex: 1 },
  caption: { color: '#718095', fontSize: 9, fontWeight: '800', letterSpacing: 0.8 },
  power: { color: '#172b36', fontSize: 22, fontWeight: '800', marginTop: 5 },
  note: { color: '#819099', fontSize: 10, marginTop: 5, maxWidth: 180 },
  statuses: { alignItems: 'flex-end', justifyContent: 'center', gap: 8 },
  commandMessage: { fontSize: 12, lineHeight: 18, marginTop: 16 },
  commandInfo: { color: '#526c77' },
  commandSuccess: { color: '#18785d', fontWeight: '700' },
  commandError: { color: '#ad4b43', fontWeight: '600' },
  controls: { flexDirection: 'row', gap: 10, marginTop: 14 },
  powerButton: { flex: 1, minHeight: 44, borderRadius: 12,
    justifyContent: 'center', alignItems: 'center' },
  onButton: { backgroundColor: '#18785d' },
  offButton: { backgroundColor: '#edf1ef', borderWidth: 1, borderColor: '#dbe3de' },
  onText: { color: '#fff', fontWeight: '800', fontSize: 14 },
  offText: { color: '#354c47', fontWeight: '800', fontSize: 14 },
  buttonDim: { opacity: 0.48 },
  disabledReason: { color: '#89959b', fontSize: 11, marginTop: 9 },
});
