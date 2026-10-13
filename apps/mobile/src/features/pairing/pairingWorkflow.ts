import { CoreApiError, isValidPairingLampName, lampsOnly } from '../../api/coreApiClient';
import type {
  PairingCandidate,
  DeviceRecord,
  PairingOperationRequest,
  PairingOperationResponse,
  PairingOperationSubmissionResponse,
  PairingSession,
  PairingSessionResponse,
  V7Id,
} from '../../api/types';

export type PairingWaitResult<T> =
  | { kind: 'ready'; value: T }
  | { kind: 'terminal'; value: T | null }
  | { kind: 'none' }
  | { kind: 'timeout'; error?: unknown }
  | { kind: 'unavailable'; error: unknown }
  | { kind: 'not_found'; error: CoreApiError }
  | { kind: 'cancelled' };

export interface PairingPollOptions {
  timeoutMs?: number;
  intervalMs?: number;
  delaysMs?: readonly number[];
  signal?: AbortSignal;
  now?: () => number;
  sleep?: (milliseconds: number, signal?: AbortSignal) => Promise<void>;
  waitUntilActive?: (signal?: AbortSignal) => Promise<void>;
  onSession?: (session: PairingSession | null) => void;
}

export interface PairingActionLock {
  active: boolean;
}

const SESSION_TERMINAL = new Set(['closed', 'expired', 'failed', 'uncertain']);
const OPERATION_TERMINAL = new Set(['succeeded', 'failed', 'rejected', 'expired', 'uncertain']);
let lastOperationId = 0;

export const isValidPairingName = isValidPairingLampName;

export function createPairingOperationId(now = Date.now, random = Math.random): V7Id {
  let id = ((now() >>> 0) ^ Math.floor(random() * 0x100000000)) >>> 0;
  if (id === 0) id = 1;
  if (id === lastOperationId) id = (id + 1) >>> 0 || 1;
  lastOperationId = id;
  return id;
}

export function acquirePairingAction(lock: PairingActionLock): boolean {
  if (lock.active) return false;
  lock.active = true;
  return true;
}

export function releasePairingAction(lock: PairingActionLock): void {
  lock.active = false;
}

export function findPairingCandidate(
  candidates: PairingCandidate[],
  hardwareId: string | null,
): PairingCandidate | null {
  return hardwareId ? candidates.find((item) => item.hardware_id === hardwareId) ?? null : null;
}

export function retainPairingSelection(
  candidates: PairingCandidate[],
  hardwareId: string | null,
): string | null {
  return findPairingCandidate(candidates, hardwareId) ? hardwareId : null;
}

export function findNewlyPairedLamp(
  devices: DeviceRecord[],
  knownIds: Set<V7Id> | null,
  name?: string,
) {
  return lampsOnly(devices).find((lamp) =>
    (!knownIds || !knownIds.has(lamp.id)) && (!name || lamp.name === name));
}

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

function isOperationNotFound(error: unknown): error is CoreApiError {
  return error instanceof CoreApiError && error.status === 404 &&
    error.code === 'operation_not_found';
}

export async function pollPairingSession(
  read: () => Promise<PairingSessionResponse>,
  until: 'active' | 'closed',
  options: PairingPollOptions = {},
): Promise<PairingWaitResult<PairingSession>> {
  const now = options.now ?? Date.now;
  const wait = options.sleep ?? defaultSleep;
  const interval = options.intervalMs ?? 700;
  const deadline = now() + (options.timeoutMs ?? 10000);
  let lastError: unknown;

  while (true) {
    if (options.signal?.aborted) return { kind: 'cancelled' };
    await options.waitUntilActive?.(options.signal);
    if (options.signal?.aborted) return { kind: 'cancelled' };
    try {
      const session = (await read()).session;
      options.onSession?.(session);
      if (until === 'active') {
        if (!session) return { kind: 'none' };
        if (session.state === 'active') return { kind: 'ready', value: session };
        if (SESSION_TERMINAL.has(session.state)) return { kind: 'terminal', value: session };
      } else {
        if (!session || session.state === 'closed' || session.state === 'expired') {
          return session ? { kind: 'ready', value: session } : { kind: 'none' };
        }
        if (session.state === 'failed' || session.state === 'uncertain') {
          return { kind: 'terminal', value: session };
        }
      }
    } catch (error) {
      if (!retryable(error)) return { kind: 'unavailable', error };
      lastError = error;
    }

    const remaining = deadline - now();
    if (remaining <= 0) return { kind: 'timeout', error: lastError };
    await wait(Math.min(interval, remaining), options.signal);
  }
}

export async function pollPairingOperation(
  id: V7Id,
  read: () => Promise<PairingOperationResponse>,
  options: PairingPollOptions = {},
): Promise<PairingWaitResult<PairingOperationResponse['operation']>> {
  const now = options.now ?? Date.now;
  const wait = options.sleep ?? defaultSleep;
  const delays = options.delaysMs?.length ? options.delaysMs : [500, 800, 1200, 1800, 2200];
  const deadline = now() + (options.timeoutMs ?? 40000);
  let delayIndex = 0;
  let lastError: unknown;

  while (true) {
    if (options.signal?.aborted) return { kind: 'cancelled' };
    await options.waitUntilActive?.(options.signal);
    if (options.signal?.aborted) return { kind: 'cancelled' };
    try {
      const operation = (await read()).operation;
      if (operation.id !== id) {
        lastError = new CoreApiError('invalid_response', 'Identifiant d’opération inattendu.');
      } else if (OPERATION_TERMINAL.has(operation.state)) {
        return { kind: 'ready', value: operation };
      }
    } catch (error) {
      if (isOperationNotFound(error)) return { kind: 'not_found', error };
      if (!retryable(error)) return { kind: 'unavailable', error };
      lastError = error;
    }

    const remaining = deadline - now();
    if (remaining <= 0) return { kind: 'timeout', error: lastError };
    const delay = delays[Math.min(delayIndex, delays.length - 1)];
    delayIndex++;
    await wait(Math.min(delay, remaining), options.signal);
  }
}

export type PairingSubmitResult =
  | { kind: 'submitted'; operation: PairingOperationSubmissionResponse['operation'] }
  | { kind: 'recovered'; operation: PairingOperationResponse['operation'] }
  | { kind: 'uncertain'; error: unknown }
  | { kind: 'cancelled' };

export async function submitPairingOperationOnce(
  request: PairingOperationRequest,
  submit: (request: PairingOperationRequest) => Promise<PairingOperationSubmissionResponse>,
  read: (id: V7Id) => Promise<PairingOperationResponse>,
  options: Pick<PairingPollOptions, 'signal' | 'sleep'> = {},
): Promise<PairingSubmitResult> {
  try {
    const response = await submit(request);
    if (response.operation.id !== request.operation_id) {
      return { kind: 'uncertain', error: new CoreApiError('invalid_response', 'Identifiant d’opération inattendu.') };
    }
    return { kind: 'submitted', operation: response.operation };
  } catch (error) {
    if (!(error instanceof CoreApiError) || error.kind !== 'submission_uncertain') throw error;
    const wait = options.sleep ?? defaultSleep;
    let lastError: unknown = error;
    for (let attempt = 0; attempt < 3; attempt++) {
      if (options.signal?.aborted) return { kind: 'cancelled' };
      try {
        const response = await read(request.operation_id);
        if (response.operation.id === request.operation_id) {
          return { kind: 'recovered', operation: response.operation };
        }
        lastError = new CoreApiError('invalid_response', 'Identifiant d’opération inattendu.');
      } catch (readError) {
        if (!isOperationNotFound(readError) && !retryable(readError)) {
          return { kind: 'uncertain', error: readError };
        }
        lastError = readError;
      }
      if (attempt < 2) await wait(500 * (attempt + 1), options.signal);
    }
    return { kind: 'uncertain', error: lastError };
  }
}

export async function openPairingSessionOnce(
  start: () => Promise<PairingSessionResponse>,
  read: () => Promise<PairingSessionResponse>,
  options: PairingPollOptions = {},
): Promise<PairingWaitResult<PairingSession>> {
  let session: PairingSession | null = null;
  try {
    session = (await start()).session;
  } catch (error) {
    if (!(error instanceof CoreApiError) || error.kind !== 'submission_uncertain') throw error;
    try {
      session = (await read()).session;
    } catch (readError) {
      return { kind: 'unavailable', error: readError };
    }
    if (!session) return { kind: 'none' };
  }
  if (!session) return { kind: 'none' };
  if (session.state === 'active') return { kind: 'ready', value: session };
  if (SESSION_TERMINAL.has(session.state)) return { kind: 'terminal', value: session };
  return pollPairingSession(read, 'active', options);
}

export async function closePairingSessionOnce(
  close: () => Promise<PairingSessionResponse>,
  read: () => Promise<PairingSessionResponse>,
  options: PairingPollOptions = {},
): Promise<PairingWaitResult<PairingSession>> {
  try {
    await close();
  } catch (error) {
    if (!(error instanceof CoreApiError) || error.kind !== 'submission_uncertain') throw error;
  }
  return pollPairingSession(read, 'closed', options);
}
