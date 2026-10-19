import type { LastConfirmedState, LampRecord } from '../../api/types';

export type BrightnessStatus = 'unknown' | 'confirmed' | 'stale';

export interface BrightnessDisplay {
  percent: number | null;
  status: BrightnessStatus;
}

export function brightnessSliderKey(lampId: number, brightness: BrightnessDisplay): string {
  return `${lampId}:${brightness.status}:${brightness.percent ?? 'unknown'}`;
}

export function brightnessDisplay(state: LastConfirmedState): BrightnessDisplay {
  const status = state.brightness_status ?? 'unknown';
  const percent = Number.isInteger(state.brightness_percent) &&
    Number(state.brightness_percent) >= 0 && Number(state.brightness_percent) <= 100
    ? Number(state.brightness_percent) : null;
  return { percent, status };
}

export function supportsBrightness(lamp: LampRecord): boolean {
  return lamp.role === 'lamp' && lamp.capabilities.includes('brightness');
}

export function canControlBrightness(lamp: LampRecord, mainOnline: boolean,
  snapshotFresh: boolean, commandBlocked: boolean): boolean {
  return supportsBrightness(lamp) && lamp.online && mainOnline && snapshotFresh &&
    !commandBlocked;
}

export function normalizeBrightnessPercent(value: number): number {
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(100, Math.round(value)));
}

export function shouldSubmitBrightnessOnRelease(
  startPercent: number,
  selectedPercent: number,
  confirmedPercent: number | null,
  confirmedStatus: BrightnessStatus,
): boolean {
  const selected = normalizeBrightnessPercent(selectedPercent);
  if (selected === normalizeBrightnessPercent(startPercent)) return false;
  return confirmedStatus !== 'confirmed' || confirmedPercent !== selected;
}
