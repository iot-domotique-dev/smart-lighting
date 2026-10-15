import type {
  CoreResponse,
  CommandState,
  CommandStatusResponse,
  DeviceRecord,
  DeviceResponse,
  DevicesResponse,
  HealthResponse,
  LampRecord,
  ModulesResponse,
  PairingCandidatesResponse,
  PairingOperationRequest,
  PairingOperationResponse,
  PairingOperationState,
  PairingOperationSummary,
  PairingOperationSubmissionResponse,
  PairingSession,
  PairingSessionResponse,
  PowerState,
  RemovalOperation,
  RemovalOperationResponse,
  RemovalOperationState,
  RemovalOperationSubmissionResponse,
  SubmitPowerResponse,
  V7Id,
} from './types';

const REQUEST_TIMEOUT_MS = 5000;

export type ApiFailureKind = 'timeout' | 'network' | 'http' | 'invalid_response' |
  'submission_uncertain';

export class CoreApiError extends Error {
  constructor(
    public readonly kind: ApiFailureKind,
    message: string,
    public readonly status?: number,
    public readonly code?: string,
  ) {
    super(message);
    this.name = 'CoreApiError';
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function isV7Id(value: unknown): value is V7Id {
  return Number.isInteger(value) && Number(value) > 0 && Number(value) <= 0xffffffff;
}

function isDeviceBase(value: unknown): value is DeviceRecord {
  return isRecord(value) && isV7Id(value.id) && typeof value.name === 'string' &&
    typeof value.role === 'string' && typeof value.online === 'boolean' &&
    (value.status === 'online' || value.status === 'offline') &&
    isV7Id(value.parent_id) && Array.isArray(value.capabilities) &&
    value.capabilities.every((item) => typeof item === 'string') &&
    typeof value.last_seen_ms === 'number' && value.state === null;
}

function isLamp(value: DeviceRecord): value is LampRecord {
  if (value.role !== 'lamp' || !isRecord(value.last_confirmed_state)) return false;
  const state = value.last_confirmed_state;
  return (state.status === 'unknown' && state.power === null) ||
    ((state.status === 'confirmed' || state.status === 'stale') &&
      (state.power === 'on' || state.power === 'off'));
}

function isDevice(value: unknown): value is DeviceRecord {
  if (!isDeviceBase(value) || !('last_confirmed_state' in value)) return false;
  return value.role === 'lamp' ? isLamp(value) : value.last_confirmed_state === null;
}

interface RequestOptions {
  method?: 'GET' | 'POST' | 'DELETE';
  token?: string;
  body?: string;
  signal?: AbortSignal;
  acceptStatuses?: readonly number[];
}

async function requestJson(ip: string, path: string, options: RequestOptions = {})
  : Promise<{ body: unknown; status: number }> {
  const method = options.method ?? 'GET';
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  const abortRequest = () => controller.abort();
  if (options.signal?.aborted) controller.abort();
  else options.signal?.addEventListener('abort', abortRequest, { once: true });
  try {
    let response: Response;
    try {
      response = await fetch(`http://${ip}/api/v1${path}`, {
        method,
        headers: {
          Accept: 'application/json',
          'Cache-Control': 'no-store',
          ...(options.token ? { Authorization: `Bearer ${options.token}` } : {}),
          ...(method === 'POST' ? { 'Content-Type': 'application/json' } : {}),
        },
        ...(options.body ? { body: options.body } : {}),
        signal: controller.signal,
      });
    } catch {
      if (method !== 'GET') {
        throw new CoreApiError('submission_uncertain',
          'Réponse perdue après envoi : la requête a peut-être été reçue par le CORE.');
      }
      if (controller.signal.aborted) {
        throw new CoreApiError('timeout', 'Le CORE ne répond pas dans le délai prévu.');
      }
      throw new CoreApiError('network', 'Connexion réseau au CORE impossible.');
    }

    let body: unknown;
    try {
      body = await response.json();
    } catch {
      if (method !== 'GET' && response.ok) {
        throw new CoreApiError('submission_uncertain',
          'Le CORE a peut-être accepté la requête, mais sa réponse est illisible.');
      }
      throw new CoreApiError('invalid_response', 'Réponse JSON illisible du CORE.');
    }
    if (!response.ok && !options.acceptStatuses?.includes(response.status)) {
      const error = isRecord(body) && isRecord(body.error) ? body.error : null;
      throw new CoreApiError(
        'http',
        'Le CORE a refusé la requête.',
        response.status,
        typeof error?.code === 'string' ? error.code : undefined,
      );
    }
    return { body, status: response.status };
  } finally {
    clearTimeout(timeout);
    options.signal?.removeEventListener('abort', abortRequest);
  }
}

async function getJson(ip: string, path: string): Promise<unknown> {
  return (await requestJson(ip, path)).body;
}

function invalidResponse(): never {
  throw new CoreApiError('invalid_response', 'Réponse inattendue du CORE.');
}

export async function getHealth(ip: string): Promise<HealthResponse> {
  const body = await getJson(ip, '/health');
  if (!isRecord(body) || body.status !== 'ok' || !isV7Id(body.core_id) ||
      typeof body.firmware_version !== 'string' || typeof body.uptime_ms !== 'number' ||
      typeof body.wifi_connected !== 'boolean' || typeof body.uart_driver_ready !== 'boolean') {
    return invalidResponse();
  }
  return body as unknown as HealthResponse;
}

export async function getCore(ip: string): Promise<CoreResponse> {
  const body = await getJson(ip, '/core');
  if (!isRecord(body) || !isRecord(body.core) || !isV7Id(body.core.id) ||
      body.core.role !== 'core' || body.core.api_version !== 'v1' ||
      typeof body.core.online !== 'boolean' ||
      typeof body.core.wifi_connected !== 'boolean' ||
      typeof body.core.uart_driver_ready !== 'boolean') {
    return invalidResponse();
  }
  return body as unknown as CoreResponse;
}

export async function getModules(ip: string): Promise<ModulesResponse> {
  const body = await getJson(ip, '/modules');
  if (!isRecord(body) || !Array.isArray(body.modules) ||
      !body.modules.every((item) => isDeviceBase(item) && item.role === 'main') ||
      typeof body.count !== 'number') return invalidResponse();
  return body as unknown as ModulesResponse;
}

export async function getDevices(ip: string): Promise<DevicesResponse> {
  const body = await getJson(ip, '/devices');
  if (!isRecord(body) || !Array.isArray(body.devices) ||
      !body.devices.every(isDevice) || typeof body.count !== 'number') return invalidResponse();
  return body as unknown as DevicesResponse;
}

export async function getDevice(ip: string, id: V7Id): Promise<DeviceResponse> {
  if (!isV7Id(id)) return invalidResponse();
  const body = await getJson(ip, `/devices/${id}`);
  if (!isRecord(body) || !isDevice(body.device) || body.device.id !== id) return invalidResponse();
  return body as unknown as DeviceResponse;
}

export async function submitPower(ip: string, id: V7Id, state: PowerState, token: string,
  signal?: AbortSignal): Promise<SubmitPowerResponse> {
  if (!isV7Id(id)) return invalidResponse();
  const { body, status } = await requestJson(ip, `/devices/${id}/commands/power`, {
    method: 'POST', token, body: JSON.stringify({ state }), signal,
  });
  if (status !== 202 || !isRecord(body) || !isRecord(body.command) ||
      !isV7Id(body.command.id) || body.command.state !== 'sent') {
    throw new CoreApiError('submission_uncertain',
      'Réponse de commande inattendue : vérifiez son résultat avant tout nouvel envoi.');
  }
  return body as unknown as SubmitPowerResponse;
}

const COMMAND_STATES: readonly CommandState[] =
  ['sent', 'accepted', 'executed', 'failed', 'expired'];

export async function getCommand(ip: string, id: V7Id, token: string,
  signal?: AbortSignal): Promise<CommandStatusResponse> {
  if (!isV7Id(id)) return invalidResponse();
  const body = (await requestJson(ip, `/commands/${id}`, { token, signal })).body;
  if (!isRecord(body) || !isRecord(body.command) || body.command.id !== id ||
      !COMMAND_STATES.includes(body.command.state as CommandState) ||
      !Number.isInteger(body.command.retries) || Number(body.command.retries) < 0 ||
      (body.command.error !== undefined && typeof body.command.error !== 'string')) {
    return invalidResponse();
  }
  return body as unknown as CommandStatusResponse;
}

const PAIRING_SESSION_STATES = [
  'opening', 'active', 'closing', 'closed', 'expired', 'failed', 'uncertain',
] as const;
const PAIRING_OPERATION_STATES: readonly PairingOperationState[] = [
  'pending', 'processing', 'succeeded', 'failed', 'rejected', 'expired', 'uncertain',
];
const REMOVAL_OPERATION_STATES: readonly RemovalOperationState[] = [
  'processing', 'succeeded', 'failed', 'uncertain', 'not_found',
];

function isRemovalOperation(value: unknown): value is RemovalOperation {
  return isRecord(value) && isV7Id(value.id) &&
    Number.isInteger(value.device_id) && Number(value.device_id) >= 0 &&
    Number(value.device_id) <= 0xffffffff &&
    REMOVAL_OPERATION_STATES.includes(value.state as RemovalOperationState) &&
    (value.result_code === undefined || typeof value.result_code === 'string');
}

export async function requestDeviceRemoval(ip: string, deviceId: V7Id,
  operationId: V7Id, token: string, signal?: AbortSignal)
  : Promise<RemovalOperationSubmissionResponse> {
  if (!isV7Id(deviceId) || !isV7Id(operationId)) return invalidResponse();

  const response = await requestJson(ip, `/devices/${deviceId}/removal-operations`, {
    method: 'POST', token, body: JSON.stringify({ operation_id: operationId }), signal,
    acceptStatuses: [200, 202, 503],
  });

  const operation = isRecord(response.body) ? response.body.operation : undefined;
  if (response.status === 503 && isRemovalOperation(operation) &&
      operation.id === operationId && operation.device_id === deviceId &&
      operation.state === 'uncertain') {
    return { operation };
  }
  if (response.status === 503 && isRecord(response.body) && isRecord(response.body.error)) {
    const error = response.body.error;
    throw new CoreApiError('http', 'Le CORE a refusé la demande de retrait.', 503,
      typeof error.code === 'string' ? error.code : undefined);
  }
  if ((response.status !== 202 && response.status !== 200) ||
      !isRemovalOperation(operation) || operation.id !== operationId ||
      operation.device_id !== deviceId) {
    throw new CoreApiError('submission_uncertain',
      'Réponse de retrait inattendue : vérifiez le même identifiant avant toute autre action.');
  }
  return { operation };
}

export async function getRemovalOperation(ip: string, operationId: V7Id,
  expectedDeviceId: V7Id, token: string, signal?: AbortSignal)
  : Promise<RemovalOperationResponse> {
  if (!isV7Id(operationId) || !isV7Id(expectedDeviceId)) return invalidResponse();
  const body = (await requestJson(ip, `/removal-operations/${operationId}`, {
    token, signal,
  })).body;
  if (!isRecord(body) || !isRemovalOperation(body.operation) ||
      body.operation.id !== operationId ||
      (body.operation.device_id !== 0 && body.operation.device_id !== expectedDeviceId) ||
      typeof body.operation.result_code !== 'string') return invalidResponse();
  return body as unknown as RemovalOperationResponse;
}

export function isValidPairingHardwareId(value: string): boolean {
  return /^C6-[0-9A-Fa-f]{16}$/.test(value);
}

export function isValidPairingLampName(value: string): boolean {
  return value.length >= 1 && value.length <= 31 && /^[A-Za-z0-9 _.'-]+$/.test(value);
}

function isPairingSession(value: unknown): value is PairingSession {
  return isRecord(value) && isV7Id(value.id) &&
    PAIRING_SESSION_STATES.includes(value.state as typeof PAIRING_SESSION_STATES[number]) &&
    (value.duration_seconds === undefined ||
      (Number.isInteger(value.duration_seconds) && Number(value.duration_seconds) >= 15 &&
        Number(value.duration_seconds) <= 180)) &&
    (value.candidate_count === undefined || (Number.isInteger(value.candidate_count) &&
      Number(value.candidate_count) >= 0 && Number(value.candidate_count) <= 10)) &&
    (value.candidate_limit_reached === undefined ||
      typeof value.candidate_limit_reached === 'boolean');
}

function isPairingOperationSummary(value: unknown, complete: boolean): value is PairingOperationSummary {
  if (!isRecord(value) || !isV7Id(value.id) || !isV7Id(value.session_id) ||
      !isValidPairingHardwareId(String(value.hardware_id)) ||
      !PAIRING_OPERATION_STATES.includes(value.state as PairingOperationState)) return false;
  if (value.name !== undefined && typeof value.name !== 'string') return false;
  if (value.result_code !== undefined && typeof value.result_code !== 'string') return false;
  if (value.local_device_id !== undefined &&
      (!Number.isInteger(value.local_device_id) || Number(value.local_device_id) < 0 ||
        Number(value.local_device_id) > 0xffffffff)) return false;
  return !complete || (typeof value.name === 'string' &&
    typeof value.result_code === 'string' && Number.isInteger(value.local_device_id));
}

export async function getPairingSession(ip: string, token: string,
  signal?: AbortSignal): Promise<PairingSessionResponse> {
  const body = (await requestJson(ip, '/pairing/session', { token, signal })).body;
  if (!isRecord(body) || !('session' in body) ||
      (body.session !== null && !isPairingSession(body.session))) return invalidResponse();
  return body as unknown as PairingSessionResponse;
}

export async function startPairingSession(ip: string, durationSeconds: number, token: string,
  signal?: AbortSignal): Promise<PairingSessionResponse> {
  if (!Number.isInteger(durationSeconds) || durationSeconds < 15 || durationSeconds > 180) {
    return invalidResponse();
  }
  const { body, status } = await requestJson(ip, '/pairing/session', {
    method: 'POST', token, body: JSON.stringify({ duration_seconds: durationSeconds }), signal,
  });
  const createdSession = isRecord(body) ? body.session : undefined;
  if (status !== 202 || !isPairingSession(createdSession) ||
      createdSession.state !== 'opening' || createdSession.duration_seconds !== durationSeconds) {
    throw new CoreApiError('submission_uncertain',
      'Réponse inattendue à la demande de recherche : relisez la session avant toute nouvelle demande.');
  }
  return body as unknown as PairingSessionResponse;
}

export async function getPairingCandidates(ip: string, token: string,
  signal?: AbortSignal): Promise<PairingCandidatesResponse> {
  const body = (await requestJson(ip, '/pairing/candidates', { token, signal })).body;
  if (!isRecord(body) || !Number.isInteger(body.session_id) || Number(body.session_id) < 0 ||
      Number(body.session_id) > 0xffffffff || typeof body.capacity_reached !== 'boolean' ||
      !Array.isArray(body.candidates) || !body.candidates.every((candidate) =>
        isRecord(candidate) && typeof candidate.hardware_id === 'string' &&
        isValidPairingHardwareId(candidate.hardware_id) &&
        Number.isInteger(candidate.capabilities) && Number(candidate.capabilities) >= 0 &&
        Number(candidate.capabilities) <= 0xffffffff && Number.isInteger(candidate.last_seen_ms) &&
        Number(candidate.last_seen_ms) >= 0 && Number(candidate.last_seen_ms) <= 0xffffffff) ||
      body.candidates.length > 10 || body.count !== body.candidates.length) {
    return invalidResponse();
  }
  return body as unknown as PairingCandidatesResponse;
}

export async function requestPairingOperation(ip: string, request: PairingOperationRequest,
  token: string, signal?: AbortSignal): Promise<PairingOperationSubmissionResponse> {
  if (!isV7Id(request.session_id) || !isV7Id(request.operation_id) ||
      !isValidPairingHardwareId(request.hardware_id) ||
      !isValidPairingLampName(request.name)) return invalidResponse();
  const { body, status } = await requestJson(ip, '/pairing/operations', {
    method: 'POST', token, body: JSON.stringify(request), signal,
  });
  const createdOperation = isRecord(body) ? body.operation : undefined;
  if ((status !== 202 && status !== 200) || !isPairingOperationSummary(createdOperation, false) ||
      createdOperation.id !== request.operation_id ||
      createdOperation.session_id !== request.session_id ||
      createdOperation.hardware_id !== request.hardware_id) {
    throw new CoreApiError('submission_uncertain',
      'Réponse inattendue à la demande d’appairage : consultez son identifiant avant toute autre action.');
  }
  return body as unknown as PairingOperationSubmissionResponse;
}

export async function getPairingOperation(ip: string, id: V7Id, token: string,
  signal?: AbortSignal): Promise<PairingOperationResponse> {
  if (!isV7Id(id)) return invalidResponse();
  const body = (await requestJson(ip, `/pairing/operations/${id}`, { token, signal })).body;
  if (!isRecord(body) || !isPairingOperationSummary(body.operation, true) ||
      body.operation.id !== id) return invalidResponse();
  return body as unknown as PairingOperationResponse;
}

export async function closePairingSession(ip: string, token: string,
  signal?: AbortSignal): Promise<PairingSessionResponse> {
  const { body, status } = await requestJson(ip, '/pairing/session', {
    method: 'DELETE', token, signal,
  });
  const closingSession = isRecord(body) ? body.session : undefined;
  if (status !== 202 || !isPairingSession(closingSession) ||
      closingSession.state !== 'closing') {
    throw new CoreApiError('submission_uncertain',
      'La fermeture n’est pas confirmée : relisez l’état de la session.');
  }
  return body as unknown as PairingSessionResponse;
}

export function lampsOnly(devices: DeviceRecord[]): LampRecord[] {
  return devices.filter(isLamp);
}

export function describeApiError(error: unknown): string {
  if (!(error instanceof CoreApiError)) return 'Une erreur inattendue est survenue.';
  if (error.kind === 'submission_uncertain') return error.message;
  if (error.kind === 'http') {
    switch (error.code) {
      case 'unauthorized': return 'Jeton refusé. Vérifiez le jeton dans les réglages.';
      case 'unknown_destination': return 'Cette lampe n’est plus connue du CORE.';
      case 'command_not_found': return 'Le suivi a disparu du CORE; le résultat est incertain.';
      case 'offline': return 'La lampe ou son MAIN est hors ligne.';
      case 'invalid_route': return 'Le CORE signale une route invalide vers cette lampe.';
      case 'command_api_disabled': return 'Le pilotage HTTP est désactivé sur le CORE.';
      case 'core_not_ready':
      case 'core_busy': return 'Le CORE n’est pas disponible pour le moment.';
      case 'transport_failure': return 'Le transport de commande du CORE est indisponible.';
      case 'tracker_full': return 'Le CORE ne peut pas suivre une autre commande actuellement.';
      case 'id_collision': return 'Le CORE n’a pas pu créer un identifiant de commande.';
      case 'pairing_session_busy': return 'Une session ou une opération d’appairage est déjà active.';
      case 'session_not_active': return 'La session de recherche n’est plus active. Actualisez son état.';
      case 'candidate_not_found': return 'Cette candidate a disparu ou n’est plus disponible.';
      case 'operation_not_found': return 'Le suivi d’appairage est introuvable; son résultat est incertain.';
      case 'operation_id_conflict': return 'Identifiant d’opération déjà utilisé pour une autre demande.';
      case 'session_not_closable': return 'La session ne peut pas être fermée maintenant. Vérifiez son état.';
      case 'pairing_in_progress': return 'Un appairage est déjà en cours.';
      case 'main_offline': return 'Le MAIN est hors ligne.';
      case 'pairing_transport_uncertain': return 'Le transport est incertain. Consultez l’état avant de réessayer.';
      case 'removal_api_disabled': return 'Le retrait est désactivé sur ce CORE.';
      case 'removal_in_progress': return 'Un retrait non résolu est déjà en cours.';
      case 'command_unresolved': return 'Une commande de cette lampe est en attente ou son résultat est incertain.';
      case 'main_unavailable': return 'Le MAIN est indisponible. Vérifiez sa connexion avant le retrait.';
      case 'unsupported_topology': return 'Le retrait mobile ne prend en charge qu’un seul MAIN.';
      case 'removal_transport_uncertain':
      case 'removal_query_uncertain': return 'La demande de retrait est incertaine. Reprenez le suivi avec le même identifiant.';
      default: break;
    }
    return `Erreur HTTP ${error.status ?? '?'}${error.code ? ` (${error.code})` : ''}.`;
  }
  return error.message;
}

export function describeCommandFailure(code?: string): string {
  if (code === 'execution_unknown') return 'Le CORE n’a pas pu confirmer l’exécution.';
  return code ? `Échec signalé par le CORE (${code}).` : 'Échec signalé par le CORE.';
}
