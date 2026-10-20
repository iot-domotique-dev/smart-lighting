import Slider from '@react-native-community/slider';
import { useRef, useState } from 'react';
import { ActivityIndicator, Pressable, StyleSheet, Text, View } from 'react-native';
import type { LampRecord, PowerState } from '../../api/types';
import { StatusIndicator } from '../../components/StatusIndicator';
import { canInterruptWithPriorityOff, isCommandInProgress } from './commandPolling';
import type { LampCommandFeedback } from './commandPolling';
import { brightnessDisplay, brightnessSliderKey, canControlBrightness, normalizeBrightnessPercent,
  shouldSubmitBrightnessOnRelease, supportsBrightness } from './brightnessControl';

function feedbackText(feedback?: LampCommandFeedback): string | null {
  if (!feedback) return null;
  const requested = feedback.brightnessPercent !== undefined
    ? `Luminosité ${feedback.brightnessPercent} %` : feedback.requested.toUpperCase();
  if (feedback.phase === 'submitting') return `Envoi de ${requested}…`;
  if (feedback.phase === 'tracking') {
    return `${requested} · ${feedback.commandState} · ${feedback.retries} reprise(s)${
      feedback.priorityOffError ? ` · OFF prioritaire refusé : ${feedback.priorityOffError}` : ''}`;
  }
  if (feedback.phase === 'refreshing') return feedback.message;
  if (feedback.phase === 'executed') {
    return `${requested} exécuté par ACK · ${feedback.retries} reprise(s)${
      feedback.refreshError ? ` · Relecture impossible : ${feedback.refreshError}` : ''}`;
  }
  return `${feedback.message}${feedback.refreshError ? ` · Relecture impossible : ${feedback.refreshError}` : ''}`;
}

export function LampCard({ lamp, mainOnline, snapshotOld, feedback,
  powerBlockedByRemoval = false, onPress, onPower, onBrightness, onReconcile }: {
  lamp: LampRecord;
  mainOnline: boolean;
  snapshotOld: boolean;
  feedback?: LampCommandFeedback;
  powerBlockedByRemoval?: boolean;
  onPress: () => void;
  onPower: (state: PowerState) => void;
  onBrightness: (percent: number) => void;
  onReconcile?: () => void;
}) {
  const history = lamp.last_confirmed_state;
  const brightness = brightnessDisplay(history);
  const brightnessEnabled = supportsBrightness(lamp);
  const power = history.power === null ? 'Inconnu' : history.power.toUpperCase();
  const status = history.status === 'confirmed' ? 'Confirmé' :
    history.status === 'stale' ? 'À vérifier' : 'Inconnu';
  const busy = isCommandInProgress(feedback);
  const canControl = !busy && !powerBlockedByRemoval && !snapshotOld && lamp.online && mainOnline;
  const canPriorityOff = canInterruptWithPriorityOff(feedback) &&
    !powerBlockedByRemoval && !snapshotOld && lamp.online && mainOnline;
  const canControlLampBrightness = canControlBrightness(lamp, mainOnline, !snapshotOld,
    busy || powerBlockedByRemoval);
  const selectionKey = brightnessSliderKey(lamp.id, brightness);
  const [selection, setSelection] = useState<{ key: string; percent: number } | null>(null);
  const dragStart = useRef<number | null>(null);

  const selectedPercent = selection?.key === selectionKey ? selection.percent : null;
  const displayedSelection = selectedPercent ?? brightness.percent;
  const sliderValue = displayedSelection;
  const brightnessStatusLabel = brightness.status === 'confirmed' ? 'confirmé' :
    brightness.status === 'stale' ? 'ancien, à vérifier' : 'inconnu';
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
      {feedback?.phase === 'uncertain' && onReconcile ? <Pressable
        style={styles.reconcileButton} onPress={onReconcile}
        accessibilityRole="button" accessibilityLabel={`Relire l’état de ${lamp.name}`}>
        <Text style={styles.reconcileText}>Relire l’état avant une nouvelle commande</Text>
      </Pressable> : null}
      <View style={styles.controls}>
        <Pressable style={({ pressed }) => [styles.powerButton, styles.onButton,
          (!canControl || pressed) && styles.buttonDim]}
          onPress={() => onPower('on')} disabled={!canControl}
          accessibilityRole="button" accessibilityLabel={`Allumer ${lamp.name}`}>
          <Text style={styles.onText}>Allumer</Text>
        </Pressable>
        <Pressable style={({ pressed }) => [styles.powerButton, styles.offButton,
          (!(canControl || canPriorityOff) || pressed) && styles.buttonDim]}
          onPress={() => onPower('off')} disabled={!(canControl || canPriorityOff)}
          accessibilityRole="button" accessibilityLabel={canPriorityOff
            ? `Éteindre en priorité ${lamp.name}` : `Éteindre ${lamp.name}`}>
          <Text style={styles.offText}>Éteindre</Text>
        </Pressable>
      </View>
      {brightnessEnabled ? <View style={styles.brightnessSection}>
        <View style={styles.brightnessHeader}>
          <Text style={styles.brightnessTitle}>Luminosité</Text>
          <Text style={styles.brightnessSelection}>{displayedSelection === null ?
            'Sélection : —' : `Sélection : ${displayedSelection} %`}</Text>
        </View>
        <Text style={styles.brightnessConfirmed}>
          {brightness.percent === null ? 'Niveau confirmé : inconnu' :
            `Dernier niveau ${brightness.status === 'stale' ? 'connu' : 'confirmé'} : ${brightness.percent} %`}
          {brightness.status === 'stale' ? ' · à vérifier' : ''}
        </Text>
        <Slider
          key={selectionKey}
          testID={`brightness-slider-${lamp.id}`}
          style={styles.slider}
          minimumValue={0}
          maximumValue={100}
          step={1}
          value={sliderValue ?? 0}
          disabled={!canControlLampBrightness}
          minimumTrackTintColor="#18785d"
          maximumTrackTintColor="#d7e0db"
          thumbTintColor="#18785d"
          onSlidingStart={(value) => { dragStart.current = normalizeBrightnessPercent(value); }}
          onValueChange={(value) => {
            if (dragStart.current === null) {
              dragStart.current = normalizeBrightnessPercent(sliderValue ?? 0);
            }
            setSelection({ key: selectionKey, percent: normalizeBrightnessPercent(value) });
          }}
          onSlidingComplete={(value) => {
            const selected = normalizeBrightnessPercent(value);
            const start = dragStart.current ?? normalizeBrightnessPercent(sliderValue ?? selected);
            dragStart.current = null;
            setSelection({ key: selectionKey, percent: selected });
            if (canControlLampBrightness && shouldSubmitBrightnessOnRelease(start, selected,
              brightness.percent, brightness.status)) onBrightness(selected);
          }}
          accessibilityLabel={`Luminosité de ${lamp.name}`}
          accessibilityValue={{ min: 0, max: 100, now: displayedSelection ?? undefined }}
        />
        <View style={styles.brightnessFooter}>
          <Text style={styles.brightnessStatus}>Consigne {brightnessStatusLabel}</Text>
          {busy ? <View style={styles.commandProgress}>
            <ActivityIndicator size="small" color="#18785d" />
            <Text style={styles.commandProgressText}>Traitement en cours</Text>
          </View> : null}
        </View>
        <Text style={styles.brightnessNote}>
          La valeur confirmée est une consigne logicielle, pas une mesure de lumière.
        </Text>
      </View> : null}
      {!canControl && !busy ? <Text style={styles.disabledReason}>
        {powerBlockedByRemoval ? 'Retrait de cette lampe en cours.' :
          snapshotOld ? 'Actualisez la disponibilité avant de commander.' :
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
  brightnessSection: { borderTopWidth: 1, borderTopColor: '#edf0ef', marginTop: 16,
    paddingTop: 14 },
  brightnessHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  brightnessTitle: { color: '#172b36', fontSize: 15, fontWeight: '800' },
  brightnessSelection: { color: '#18785d', fontSize: 13, fontWeight: '800' },
  brightnessConfirmed: { color: '#718095', fontSize: 12, marginTop: 5 },
  slider: { width: '100%', height: 42, marginTop: 6 },
  brightnessFooter: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  brightnessStatus: { color: '#637480', fontSize: 11 },
  commandProgress: { flexDirection: 'row', alignItems: 'center', gap: 7 },
  commandProgressText: { color: '#526c77', fontSize: 11 },
  brightnessNote: { color: '#89959b', fontSize: 10, marginTop: 7, lineHeight: 15 },
  reconcileButton: { backgroundColor: '#fff0d8', borderRadius: 10, padding: 10, marginTop: 10 },
  reconcileText: { color: '#825718', fontSize: 11, fontWeight: '700', textAlign: 'center' },
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
