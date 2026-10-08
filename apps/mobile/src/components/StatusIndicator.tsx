import { StyleSheet, Text, View } from 'react-native';

type Tone = 'good' | 'warn' | 'bad' | 'neutral';

const colors: Record<Tone, string> = {
  good: '#17805f',
  warn: '#a56b19',
  bad: '#bd4b45',
  neutral: '#718095',
};

export function StatusIndicator({ label, tone, onDark = false }: {
  label: string; tone: Tone; onDark?: boolean;
}) {
  const color = onDark ? '#d5e9df' : colors[tone];
  return (
    <View style={styles.row}>
      <View style={[styles.dot, { backgroundColor: colors[tone] }]} />
      <Text style={[styles.label, { color }]}>{label}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  row: { flexDirection: 'row', alignItems: 'center', gap: 6 },
  dot: { width: 7, height: 7, borderRadius: 4 },
  label: { fontSize: 12, fontWeight: '700' },
});
