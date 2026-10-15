import * as SecureStore from 'expo-secure-store';
import { normalizeLocalIp } from '../../settings/connectionSettings';
import type { V7Id } from '../../api/types';

const REMOVAL_KEY = 'smart_lighting_pending_removal';

export type StoredRemovalPhase = 'submitting' | 'processing' | 'uncertain' |
  'not_found' | 'succeeded' | 'failed';

export interface PendingRemovalOperation {
  operationId: V7Id;
  deviceId: V7Id;
  deviceName: string;
  coreIp: string;
  phase: StoredRemovalPhase;
}

function isV7Id(value: unknown): value is V7Id {
  return Number.isInteger(value) && Number(value) > 0 && Number(value) <= 0xffffffff;
}

function isStoredRemoval(value: unknown): value is PendingRemovalOperation {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
  const record = value as Record<string, unknown>;
  return isV7Id(record.operationId) && isV7Id(record.deviceId) &&
    typeof record.deviceName === 'string' && record.deviceName.length > 0 &&
    typeof record.coreIp === 'string' && normalizeLocalIp(record.coreIp) === record.coreIp &&
    ['submitting', 'processing', 'uncertain', 'not_found', 'succeeded', 'failed']
      .includes(String(record.phase));
}

export async function loadPendingRemoval(): Promise<PendingRemovalOperation | null> {
  const raw = await SecureStore.getItemAsync(REMOVAL_KEY);
  if (raw === null) return null;
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    throw new Error('La sauvegarde du retrait est illisible. Ne démarrez pas un autre retrait.');
  }
  if (!isStoredRemoval(parsed)) {
    throw new Error('La sauvegarde du retrait est invalide. Ne démarrez pas un autre retrait.');
  }
  return parsed;
}

export async function savePendingRemoval(operation: PendingRemovalOperation): Promise<void> {
  if (!isStoredRemoval(operation)) throw new Error('Identifiants de retrait invalides.');
  await SecureStore.setItemAsync(REMOVAL_KEY, JSON.stringify(operation));
}

export async function clearPendingRemoval(): Promise<void> {
  await SecureStore.deleteItemAsync(REMOVAL_KEY);
}
