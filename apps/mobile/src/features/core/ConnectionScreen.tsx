import { useState } from 'react';
import { ActivityIndicator, Alert, Pressable, ScrollView, StyleSheet, Text,
  TextInput, View } from 'react-native';
import { describeApiError, getCore, getHealth } from '../../api/coreApiClient';
import { normalizeLocalIp } from '../../settings/connectionSettings';

interface Props {
  savedIp?: string;
  hasSavedToken: boolean;
  onSave: (ip: string, token: string) => Promise<void>;
  onCancel?: () => void;
  onDelete?: () => Promise<void>;
}

export function ConnectionScreen({ savedIp, hasSavedToken, onSave, onCancel, onDelete }: Props) {
  const [ip, setIp] = useState(savedIp ?? '');
  const [token, setToken] = useState('');
  const [checking, setChecking] = useState(false);
  const [saving, setSaving] = useState(false);
  const [message, setMessage] = useState('');
  const [success, setSuccess] = useState(false);
  const busy = checking || saving;

  async function testConnection() {
    const address = normalizeLocalIp(ip);
    if (!address) {
      setSuccess(false);
      setMessage('Saisissez une adresse IPv4 privée du réseau local.');
      return;
    }
    setChecking(true);
    setMessage('');
    try {
      const [health, core] = await Promise.all([getHealth(address), getCore(address)]);
      if (health.core_id !== core.core.id) throw new Error('Identité CORE incohérente.');
      setSuccess(true);
      setMessage(`CORE joignable · ID ${core.core.id} · UART ${health.uart_driver_ready ? 'prêt' : 'non prêt'}.`);
    } catch (error) {
      setSuccess(false);
      setMessage(describeApiError(error));
    } finally {
      setChecking(false);
    }
  }

  async function save() {
    const address = normalizeLocalIp(ip);
    if (!address) {
      setSuccess(false);
      setMessage('Saisissez une adresse IPv4 privée du réseau local.');
      return;
    }
    if (!token.trim() && !hasSavedToken) {
      setSuccess(false);
      setMessage('Saisissez le jeton Bearer du CORE.');
      return;
    }
    if (token.trim() && (token.trim().length < 32 || token.trim().length > 128)) {
      setSuccess(false);
      setMessage('Le jeton doit comporter entre 32 et 128 caractères.');
      return;
    }
    setSaving(true);
    setMessage('');
    try {
      await onSave(address, token);
      setToken('');
    } catch {
      setSuccess(false);
      setMessage('Impossible d’enregistrer les paramètres sur ce téléphone.');
    } finally {
      setSaving(false);
    }
  }

  async function clear() {
    if (!onDelete) return;
    setSaving(true);
    try {
      await onDelete();
      setToken('');
    } catch {
      setSuccess(false);
      setMessage('Impossible de supprimer les paramètres enregistrés.');
    } finally {
      setSaving(false);
    }
  }

  function confirmClear() {
    Alert.alert('Supprimer la connexion ?', 'L’adresse et le jeton enregistrés seront effacés de ce téléphone.', [
      { text: 'Annuler', style: 'cancel' },
      { text: 'Supprimer', style: 'destructive', onPress: () => void clear() },
    ]);
  }

  return (
    <ScrollView contentContainerStyle={styles.container} keyboardShouldPersistTaps="handled">
      <View style={styles.brand}><Text style={styles.brandText}>SL</Text></View>
      <Text style={styles.eyebrow}>SMART LIGHTING · LOCAL</Text>
      <Text style={styles.title}>Connexion au CORE</Text>
      <Text style={styles.intro}>Connectez votre téléphone au même réseau local que CORE-WIFI.</Text>

      <View style={styles.card}>
        <Text style={styles.label}>ADRESSE IP DU CORE-WIFI</Text>
        <TextInput style={styles.input} value={ip} onChangeText={setIp}
          placeholder="192.168.1.10" placeholderTextColor="#94a2aa"
          keyboardType="decimal-pad" autoCapitalize="none" autoCorrect={false}
          editable={!busy} accessibilityLabel="Adresse IP locale du CORE" />
        <Text style={styles.hint}>Adresse IPv4 privée uniquement · port 80</Text>

        <Text style={[styles.label, styles.tokenLabel]}>JETON BEARER</Text>
        <TextInput style={styles.input} value={token} onChangeText={setToken}
          placeholder={hasSavedToken ? 'Jeton enregistré · laisser vide pour conserver' : 'Saisir le jeton'}
          placeholderTextColor="#94a2aa" secureTextEntry autoCapitalize="none"
          autoCorrect={false} editable={!busy} accessibilityLabel="Jeton Bearer" />
        <Text style={styles.hint}>Conservé dans le stockage sécurisé du téléphone.</Text>

        {message ? <View style={[styles.message, success ? styles.messageOk : styles.messageError]}>
          <Text style={styles.messageText}>{message}</Text>
        </View> : null}

        <Pressable style={[styles.primary, busy && styles.disabled]} onPress={save} disabled={busy}
          accessibilityRole="button">
          {saving ? <ActivityIndicator color="#fff" /> :
            <Text style={styles.primaryText}>Enregistrer et continuer</Text>}
        </Pressable>
        <Pressable style={[styles.secondary, busy && styles.disabled]} onPress={testConnection}
          disabled={busy} accessibilityRole="button">
          {checking ? <ActivityIndicator color="#20745b" /> :
            <Text style={styles.secondaryText}>Tester la connexion</Text>}
        </Pressable>
      </View>

      <Text style={styles.footnote}>Le test vérifie HTTP et l’identité du CORE. Il ne valide ni le jeton ni la liaison Zigbee.</Text>
      {onCancel ? <Pressable onPress={onCancel} disabled={busy} style={styles.textButton}>
        <Text style={styles.textButtonLabel}>Retour aux lampes</Text>
      </Pressable> : null}
      {onDelete ? <Pressable onPress={confirmClear} disabled={busy} style={styles.textButton}>
        <Text style={[styles.textButtonLabel, styles.delete]}>Supprimer les paramètres enregistrés</Text>
      </Pressable> : null}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: { flexGrow: 1, backgroundColor: '#f4f7f4', padding: 24, paddingTop: 42 },
  brand: { width: 48, height: 48, backgroundColor: '#18785d', borderRadius: 16,
    alignItems: 'center', justifyContent: 'center', marginBottom: 34 },
  brandText: { color: '#fff', fontSize: 17, fontWeight: '900' },
  eyebrow: { color: '#17805f', fontSize: 11, fontWeight: '800', letterSpacing: 2 },
  title: { color: '#172b36', fontSize: 31, fontWeight: '800', marginTop: 10 },
  intro: { color: '#637480', fontSize: 15, lineHeight: 22, marginTop: 10, marginBottom: 26 },
  card: { backgroundColor: '#fff', borderRadius: 22, padding: 20,
    borderWidth: 1, borderColor: '#e4ebe5' },
  label: { color: '#425462', fontSize: 11, fontWeight: '800', letterSpacing: 0.8 },
  tokenLabel: { marginTop: 22 },
  input: { marginTop: 9, borderWidth: 1, borderColor: '#dce5de', backgroundColor: '#f8faf8',
    borderRadius: 12, height: 50, paddingHorizontal: 14, fontSize: 15, color: '#172b36' },
  hint: { color: '#83919a', fontSize: 11, marginTop: 8 },
  message: { padding: 12, borderRadius: 10, marginTop: 22 },
  messageOk: { backgroundColor: '#e7f4ec' },
  messageError: { backgroundColor: '#ffefea' },
  messageText: { color: '#29423f', fontSize: 12, lineHeight: 18 },
  primary: { backgroundColor: '#18785d', borderRadius: 12, padding: 15,
    alignItems: 'center', marginTop: 25, minHeight: 50 },
  primaryText: { color: '#fff', fontSize: 15, fontWeight: '800' },
  secondary: { borderWidth: 1, borderColor: '#c7ddd0', borderRadius: 12,
    alignItems: 'center', padding: 14, marginTop: 10, minHeight: 50 },
  secondaryText: { color: '#20745b', fontSize: 15, fontWeight: '700' },
  disabled: { opacity: 0.55 },
  footnote: { color: '#7b8b93', fontSize: 12, lineHeight: 18, marginTop: 20 },
  textButton: { paddingVertical: 15, alignItems: 'center' },
  textButtonLabel: { color: '#20745b', fontWeight: '700', fontSize: 13 },
  delete: { color: '#b64e49' },
});
