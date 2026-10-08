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
  PowerState,
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
  method?: 'GET' | 'POST';
  token?: string;
  body?: string;
  signal?: AbortSignal;
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
      if (method === 'POST') {
        throw new CoreApiError('submission_uncertain',
          'Réponse perdue après envoi : la commande a peut-être été reçue par le CORE.');
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
      if (method === 'POST' && response.ok) {
        throw new CoreApiError('submission_uncertain',
          'Le CORE a peut-être accepté la commande, mais sa réponse est illisible.');
      }
      throw new CoreApiError('invalid_response', 'Réponse JSON illisible du CORE.');
    }
    if (!response.ok) {
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
