import * as SecureStore from 'expo-secure-store';
import { normalizeLocalIp } from '../../settings/connectionSettings';
import type { PowerState, V7Id } from '../../api/types';

const GROUP_OPERATION_KEY = 'smart_lighting_pending_group_power';

export interface PendingGroupPowerOperation {
  operationId: V7Id;
  mainId: V7Id;
  groupId: V7Id;
  power: PowerState;
  coreIp: string;
}

function isV7Id(value: unknown): value is V7Id {
  return Number.isInteger(value) && Number(value) > 0 && Number(value) <= 0xffffffff;
}

function isPendingGroupPowerOperation(value: unknown): value is PendingGroupPowerOperation {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
  const record = value as Record<string, unknown>;
  return isV7Id(record.operationId) && isV7Id(record.mainId) &&
    isV7Id(record.groupId) && (record.power === 'on' || record.power === 'off') &&
    typeof record.coreIp === 'string' && normalizeLocalIp(record.coreIp) === record.coreIp;
}

export async function loadPendingGroupPowerOperation()
  : Promise<PendingGroupPowerOperation | null> {
  const raw = await SecureStore.getItemAsync(GROUP_OPERATION_KEY);
  if (raw === null) return null;
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    throw new Error('La sauvegarde de la commande de groupe est illisible. Ne renvoyez pas la commande.');
  }
  if (!isPendingGroupPowerOperation(parsed)) {
    throw new Error('La sauvegarde de la commande de groupe est invalide. Ne renvoyez pas la commande.');
  }
  return parsed;
}

export async function savePendingGroupPowerOperation(
  operation: PendingGroupPowerOperation,
): Promise<void> {
  if (!isPendingGroupPowerOperation(operation)) {
    throw new Error('Identifiants de commande de groupe invalides.');
  }
  await SecureStore.setItemAsync(GROUP_OPERATION_KEY, JSON.stringify(operation));
}

export async function clearPendingGroupPowerOperation(): Promise<void> {
  await SecureStore.deleteItemAsync(GROUP_OPERATION_KEY);
}
