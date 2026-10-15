export type V7Id = number;
export type PowerState = 'on' | 'off';

export type LastConfirmedState =
  | { power: null; status: 'unknown' }
  | { power: PowerState; status: 'confirmed' | 'stale' };

export interface HealthResponse {
  status: 'ok';
  core_id: V7Id;
  firmware_version: string;
  uptime_ms: number;
  wifi_connected: boolean;
  uart_driver_ready: boolean;
}

export interface CoreResponse {
  core: {
    id: V7Id;
    name: string;
    role: 'core';
    status: 'online' | 'offline';
    online: boolean;
    api_version: 'v1';
    firmware_version: string;
    uptime_ms: number;
    wifi_connected: boolean;
    uart_driver_ready: boolean;
  };
}

export interface DeviceBase {
  id: V7Id;
  name: string;
  role: string;
  online: boolean;
  status: 'online' | 'offline';
  parent_id: V7Id;
  capabilities: string[];
  last_seen_ms: number;
  state: null;
}

export interface ModuleRecord extends DeviceBase {
  role: 'main';
}

export interface DeviceRecord extends DeviceBase {
  last_confirmed_state: LastConfirmedState | null;
}

export interface LampRecord extends DeviceRecord {
  role: 'lamp';
  last_confirmed_state: LastConfirmedState;
}

export interface ModulesResponse {
  modules: ModuleRecord[];
  count: number;
}

export interface DevicesResponse {
  devices: DeviceRecord[];
  count: number;
}

export interface DeviceResponse {
  device: DeviceRecord;
}

// Contrat existant, utilisé par l'interface de commandes à partir de V7.5.2.
export interface SubmitPowerResponse {
  command: { id: V7Id; state: 'sent' };
}

export type CommandState = 'sent' | 'accepted' | 'executed' | 'failed' | 'expired';

export interface CommandStatusResponse {
  command: {
    id: V7Id;
    state: CommandState;
    retries: number;
    error?: string;
  };
}

export type PairingSessionState = 'opening' | 'active' | 'closing' | 'closed' |
  'expired' | 'failed' | 'uncertain';

export interface PairingSession {
  id: V7Id;
  state: PairingSessionState;
  duration_seconds?: number;
  candidate_count?: number;
  candidate_limit_reached?: boolean;
}

export interface PairingSessionResponse {
  session: PairingSession | null;
}

export interface PairingCandidate {
  hardware_id: string;
  capabilities: number;
  last_seen_ms: number;
}

export interface PairingCandidatesResponse {
  session_id: number;
  capacity_reached: boolean;
  candidates: PairingCandidate[];
  count: number;
}

export type PairingOperationState = 'pending' | 'processing' | 'succeeded' |
  'failed' | 'rejected' | 'expired' | 'uncertain';

export interface PairingOperationRequest {
  session_id: V7Id;
  operation_id: V7Id;
  hardware_id: string;
  name: string;
}

export interface PairingOperationSummary {
  id: V7Id;
  session_id: V7Id;
  hardware_id: string;
  state: PairingOperationState;
  name?: string;
  result_code?: string;
  local_device_id?: number;
}

export interface PairingOperationSubmissionResponse {
  operation: PairingOperationSummary;
}

export interface PairingOperationResponse {
  operation: PairingOperationSummary & {
    name: string;
    result_code: string;
    local_device_id: number;
  };
}

export type RemovalOperationState = 'processing' | 'succeeded' | 'failed' |
  'uncertain' | 'not_found';

export interface RemovalOperation {
  id: V7Id;
  device_id: V7Id;
  state: RemovalOperationState;
  result_code?: string;
}

export interface RemovalOperationResponse {
  operation: RemovalOperation & { result_code: string };
}

export interface RemovalOperationSubmissionResponse {
  operation: RemovalOperation;
}

export interface ApiErrorResponse {
  error: { code: string; message: string };
}
