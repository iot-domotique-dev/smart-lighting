import type { GroupMemberOperationResult, GroupOperation } from '../../api/types';

const operationStates: Record<GroupOperation['state'], string> = {
  sent: 'envoyée',
  accepted: 'acceptée par MAIN',
  persisted: 'enregistrée',
  completed: 'terminée',
  failed: 'échouée',
  uncertain: 'incertaine',
};

const resultLabels: Record<string, string> = {
  accepted: 'acceptée',
  busy: 'MAIN occupé',
  completed: 'toutes les lampes ont confirmé',
  empty_group: 'groupe vide',
  failed: 'échec',
  invalid: 'demande invalide',
  not_found: 'groupe introuvable',
  offline: 'lampe hors ligne',
  partial: 'résultat partiel',
  persisted: 'enregistrée',
  storage_unavailable: 'stockage MAIN indisponible',
  uncertain: 'résultat incertain',
  unsupported: 'capacité absente',
};

const memberStates: Record<GroupMemberOperationResult['state'], string> = {
  in_progress: 'en cours',
  executed: 'exécutée et confirmée',
  failed: 'échec confirmé',
  offline: 'hors ligne',
  unsupported: 'capacité absente',
  uncertain: 'résultat incertain',
};

export function groupOperationStateLabel(state: GroupOperation['state']): string {
  return operationStates[state];
}

export function groupOperationResultLabel(result?: string): string | undefined {
  if (result === undefined) return undefined;
  return resultLabels[result] ?? result;
}

export function groupMemberResultLabel(member: GroupMemberOperationResult): string {
  const state = memberStates[member.state];
  if (member.result === 'superseded') return `${state} · remplacée par OFF prioritaire`;
  if (member.result === 'in_progress' || member.result === member.state) return state;
  return `${state} · ${resultLabels[member.result] ?? member.result}`;
}
