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

export interface ApiErrorResponse {
  error: { code: string; message: string };
}
