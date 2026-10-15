import { CoreApiError } from '../../api/coreApiClient';
import type { RemovalOperationResponse, V7Id } from '../../api/types';

export interface RemovalActionLock {
  active: boolean;
}

export interface RemovalAvailability {
  lampOnline: boolean;
  mainOnline: boolean;
  inventoryFresh: boolean;
  commandPhase?: string;
  hasPendingRemoval: boolean;
  storageAvailable: boolean;
  actionLocked: boolean;
}

export interface RemovalPollOptions {
  timeoutMs?: number;
  delaysMs?: readonly number[];
  signal?: AbortSignal;
  now?: () => number;
  sleep?: (milliseconds: number, signal?: AbortSignal) => Promise<void>;
  waitUntilActive?: (signal?: AbortSignal) => Promise<void>;
  onOperation?: (operation: RemovalOperationResponse['operation']) => void;
}

export type RemovalPollResult =
  | { kind: 'succeeded'; operation: RemovalOperationResponse['operation'] }
  | { kind: 'failed'; operation: RemovalOperationResponse['operation'] }
  | { kind: 'not_found'; operation: RemovalOperationResponse['operation'] }
  | { kind: 'uncertain'; operation?: RemovalOperationResponse['operation']; error?: unknown }
  | { kind: 'unavailable'; error: unknown }
  | { kind: 'cancelled' };

const DEFAULT_TIMEOUT_MS = 60000;
const DEFAULT_DELAYS_MS = [800, 1400, 2000, 2500] as const;

function defaultSleep(milliseconds: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const finish = () => {
      if (timer !== undefined) clearTimeout(timer);
      signal?.removeEventListener('abort', finish);
      resolve();
    };
    if (signal?.aborted) return resolve();
    timer = setTimeout(finish, milliseconds);
    signal?.addEventListener('abort', finish, { once: true });
  });
}

function retryable(error: unknown): boolean {
  if (!(error instanceof CoreApiError)) return true;
  if (error.kind === 'timeout' || error.kind === 'network' ||
      error.kind === 'invalid_response') return true;
  return error.kind === 'http' && (error.status === 503 || (error.status ?? 0) >= 500);
}

export async function createRemovalOperationId(
  randomBytes: (count: number) => Promise<Uint8Array>,
): Promise<V7Id> {
  for (let attempt = 0; attempt < 4; attempt++) {
    const bytes = await randomBytes(4);
    if (!(bytes instanceof Uint8Array) || bytes.length !== 4) {
      throw new Error('Le générateur aléatoire n’a pas fourni 4 octets.');
    }
    const id = bytes[0] * 0x1000000 + bytes[1] * 0x10000 +
      bytes[2] * 0x100 + bytes[3];
    if (id !== 0) return id;
  }
  throw new Error('Impossible de générer un identifiant de retrait non nul.');
}

export function acquireRemovalAction(lock: RemovalActionLock): boolean {
  if (lock.active) return false;
  lock.active = true;
  return true;
}

export function releaseRemovalAction(lock: RemovalActionLock): void {
  lock.active = false;
}

export function canRequestRemoval(availability: RemovalAvailability): boolean {
  return availability.lampOnline && availability.mainOnline &&
    availability.inventoryFresh && !isCommandBlockingRemoval(availability.commandPhase) &&
    !availability.hasPendingRemoval && availability.storageAvailable &&
    !availability.actionLocked;
}

export function createRemovalConfirmation(deviceName: string, deviceId: V7Id)
  : { title: string; message: string } {
  return {
    title: 'Retirer cette lampe ?',
    message: `Nom : ${deviceName}\nIdentifiant : ${deviceId}\n\nElle sera retirée de Smart Lighting et pourra être ajoutée à nouveau ultérieurement. Ce retrait n’éteint pas la sortie électrique.`,
  };
}

export function isRemovalBlockingCommand(deviceId: V7Id, removalDeviceId?: V7Id): boolean {
  return removalDeviceId === deviceId;
}

export function isCommandBlockingRemoval(phase?: string): boolean {
  return phase === 'submitting' || phase === 'tracking' || phase === 'refreshing' ||
    phase === 'uncertain';
}

export function isRemovalInProgress(state: string): boolean {
  return state === 'processing' || state === 'uncertain';
}

export async function pollRemovalOperation(
  operationId: V7Id,
  read: () => Promise<RemovalOperationResponse>,
  options: RemovalPollOptions = {},
): Promise<RemovalPollResult> {
  const now = options.now ?? Date.now;
  const wait = options.sleep ?? defaultSleep;
  const delays = options.delaysMs?.length ? options.delaysMs : DEFAULT_DELAYS_MS;
  const deadline = now() + (options.timeoutMs ?? DEFAULT_TIMEOUT_MS);
  let delayIndex = 0;
  let lastError: unknown;
  let lastOperation: RemovalOperationResponse['operation'] | undefined;

  while (true) {
    if (options.signal?.aborted) return { kind: 'cancelled' };
    await options.waitUntilActive?.(options.signal);
    if (options.signal?.aborted) return { kind: 'cancelled' };

    try {
      const operation = (await read()).operation;
      if (operation.id !== operationId) {
        lastError = new CoreApiError('invalid_response', 'Identifiant de retrait inattendu.');
      } else {
        lastOperation = operation;
        options.onOperation?.(operation);
        if (operation.state === 'succeeded') return { kind: 'succeeded', operation };
        if (operation.state === 'failed') return { kind: 'failed', operation };
        if (operation.state === 'not_found') return { kind: 'not_found', operation };
      }
    } catch (error) {
      if (!retryable(error)) return { kind: 'unavailable', error };
      lastError = error;
    }

    const remaining = deadline - now();
    if (remaining <= 0) return { kind: 'uncertain', operation: lastOperation, error: lastError };
    const delay = delays[Math.min(delayIndex, delays.length - 1)];
    delayIndex++;
    await wait(Math.min(delay, remaining), options.signal);
  }
}
