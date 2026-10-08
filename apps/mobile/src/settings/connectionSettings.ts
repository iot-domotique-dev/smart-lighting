import * as SecureStore from 'expo-secure-store';
import { tokenStore } from '../security/tokenStore';

const IP_KEY = 'smart_lighting_core_ip';

export interface ConnectionSettings {
  ip: string;
  token: string;
}

export function normalizeLocalIp(input: string): string | null {
  const parts = input.trim().split('.');
  if (parts.length !== 4 || !parts.every((part) => /^\d{1,3}$/.test(part) && Number(part) <= 255)) {
    return null;
  }
  const numbers = parts.map(Number);
  const local = numbers[0] === 10 ||
    (numbers[0] === 172 && numbers[1] >= 16 && numbers[1] <= 31) ||
    (numbers[0] === 192 && numbers[1] === 168);
  return local ? numbers.join('.') : null;
}

export async function loadConnection(): Promise<ConnectionSettings | null> {
  const [ip, token] = await Promise.all([
    SecureStore.getItemAsync(IP_KEY),
    tokenStore.read(),
  ]);
  return ip && token && normalizeLocalIp(ip) === ip &&
    token.length >= 32 && token.length <= 128 ? { ip, token } : null;
}

export async function saveConnection(settings: ConnectionSettings): Promise<void> {
  const ip = normalizeLocalIp(settings.ip);
  if (!ip || settings.token.trim().length < 32 || settings.token.trim().length > 128) {
    throw new Error('Adresse locale et jeton de 32 à 128 caractères requis.');
  }
  await tokenStore.save(settings.token.trim());
  await SecureStore.setItemAsync(IP_KEY, ip);
}

export async function clearConnection(): Promise<void> {
  await Promise.all([SecureStore.deleteItemAsync(IP_KEY), tokenStore.clear()]);
}
