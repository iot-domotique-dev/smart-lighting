import { CoreApiError } from '../../api/coreApiClient';
import type { CommandStatusResponse, PowerState, V7Id } from '../../api/types';

export type LampCommandFeedback =
  | { phase: 'submitting'; requested: PowerState }
  | { phase: 'tracking'; requested: PowerState; commandId: V7Id;
      commandState: 'sent' | 'accepted'; retries: number }
  | { phase: 'refreshing'; requested: PowerState; message: string }
  | { phase: 'executed'; requested: PowerState; commandId: V7Id; retries: number;
      refreshError?: string }
  | { phase: 'failed'; requested: PowerState; commandId: V7Id; retries: number;
      message: string; refreshError?: string }
  | { phase: 'uncertain'; requested: PowerState; commandId?: V7Id; retries?: number;
      message: string; refreshError?: string }
  | { phase: 'rejected'; requested: PowerState; message: string; refreshError?: string };

export type CommandPollResult =
  | { kind: 'terminal'; command: CommandStatusResponse['command'] }
  | { kind: 'not_found'; error: CoreApiError }
  | { kind: 'unavailable'; error: unknown }
  | { kind: 'timeout'; lastError?: unknown }
  | { kind: 'cancelled' };

export interface CommandPollOptions {
  timeoutMs?: number;
  delaysMs?: readonly number[];
  signal?: AbortSignal;
  now?: () => number;
  sleep?: (milliseconds: number, signal?: AbortSignal) => Promise<void>;
  waitUntilActive?: (signal?: AbortSignal) => Promise<void>;
  onStatus?: (command: CommandStatusResponse['command']) => void;
}

const DEFAULT_TIMEOUT_MS = 30000;
const DEFAULT_DELAYS_MS = [500, 800, 1200, 1800, 2500] as const;

function sleep(milliseconds: number, signal?: AbortSignal): Promise<void> {
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
  if (error.kind === 'http' && error.status === 503 &&
      error.code === 'command_api_disabled') return false;
  return error.kind === 'http' && (error.status === 503 || (error.status ?? 0) >= 500);
}

export function isCommandInProgress(feedback?: LampCommandFeedback): boolean {
  return feedback?.phase === 'submitting' || feedback?.phase === 'tracking' ||
    feedback?.phase === 'refreshing';
}

export function acquireLampCommand(lockedIds: Set<V7Id>, lampId: V7Id): boolean {
  if (lockedIds.has(lampId)) return false;
  lockedIds.add(lampId);
  return true;
}

export function releaseLampCommand(lockedIds: Set<V7Id>, lampId: V7Id): void {
  lockedIds.delete(lampId);
}

export async function pollCommand(
  commandId: V7Id,
  readStatus: () => Promise<CommandStatusResponse>,
  options: CommandPollOptions = {},
): Promise<CommandPollResult> {
  const now = options.now ?? Date.now;
  const wait = options.sleep ?? sleep;
  const delays = options.delaysMs?.length ? options.delaysMs : DEFAULT_DELAYS_MS;
  const deadline = now() + (options.timeoutMs ?? DEFAULT_TIMEOUT_MS);
  let delayIndex = 0;
  let lastError: unknown;

  while (true) {
    if (options.signal?.aborted) return { kind: 'cancelled' };
    await options.waitUntilActive?.(options.signal);
    if (options.signal?.aborted) return { kind: 'cancelled' };

    try {
      const response = await readStatus();
      const command = response.command;
      if (command.id !== commandId) {
        lastError = new CoreApiError('invalid_response', 'Le CORE a répondu avec un autre ID de commande.');
      } else {
        options.onStatus?.(command);
        if (command.state === 'executed' || command.state === 'failed' || command.state === 'expired') {
          return { kind: 'terminal', command };
        }
      }
    } catch (error) {
      if (error instanceof CoreApiError && error.status === 404 &&
          error.code === 'command_not_found') return { kind: 'not_found', error };
      if (!retryable(error)) return { kind: 'unavailable', error };
      lastError = error;
    }

    const remaining = deadline - now();
    if (remaining <= 0) return { kind: 'timeout', lastError };
    const delay = delays[Math.min(delayIndex, delays.length - 1)];
    delayIndex++;
    await wait(Math.min(delay, remaining), options.signal);
  }
}
