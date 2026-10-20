const assert = require('node:assert/strict');
const fs = require('node:fs');
const Module = require('node:module');
const path = require('node:path');
const { test } = require('node:test');
const ts = require('typescript');

require.extensions['.ts'] = (module, filename) => {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  module._compile(output, filename);
};

const workflow = require('../src/features/lighting/removalWorkflow.ts');
const api = require('../src/api/coreApiClient.ts');
const viewSource = fs.readFileSync(path.join(__dirname,
  '../src/features/lighting/LampListScreen.tsx'), 'utf8');

function operation(state, id = 1234, deviceId = 11) {
  return { operation: { id, device_id: deviceId, state, result_code: state } };
}

test('détail expose une action de retrait séparée et exige la confirmation explicite', () => {
  assert.match(viewSource, /accessibilityLabel="Retirer cette lampe"/);
  assert.match(viewSource, /Alert\.alert\(confirmation\.title, confirmation\.message, \[/);
  assert.match(viewSource, /\{ text: 'Annuler', style: 'cancel' \}/);
  assert.match(viewSource, /\{ text: 'Confirmer le retrait', style: 'destructive',[\s\S]*onPress: \(\) => void confirmRemovalRequest\(lamp\)/);
  assert.deepEqual(workflow.createRemovalConfirmation('Lampe salon', 11), {
    title: 'Retirer cette lampe ?',
    message: 'Nom : Lampe salon\nIdentifiant : 11\n\nElle sera retirée de Smart Lighting et pourra être ajoutée à nouveau ultérieurement. Avant le désappairage, la lampe tentera d’arrêter un fade actif et de forcer sa sortie GPIO à l’état bas. Si cette extinction ne peut pas être confirmée par le pilote, le retrait sera refusé. Cela ne mesure pas l’état électrique de la LED.',
  });
});

test('Annuler n’a aucun callback et le flux n’effectue le POST que depuis Confirmer', () => {
  const alertButtons = viewSource.match(/\{ text: 'Annuler', style: 'cancel' \}/);
  assert.ok(alertButtons);
  assert.doesNotMatch(alertButtons[0], /onPress/);
  assert.match(viewSource, /requestDeviceRemoval\(ip, lamp\.id, operationId, token\)/);
  assert.match(viewSource, /await savePendingRemoval\(operation\);[\s\S]*requestDeviceRemoval/);
});

test('operation_id utilise quatre octets aléatoires, reste uint32 et n’est jamais nul', async () => {
  let calls = 0;
  const id = await workflow.createRemovalOperationId(async (count) => {
    calls++;
    assert.equal(count, 4);
    return calls === 1 ? new Uint8Array([0, 0, 0, 0]) :
      new Uint8Array([255, 255, 255, 255]);
  });
  assert.equal(id, 0xffffffff);
  assert.equal(calls, 2);
  assert.ok(Number.isInteger(id) && id > 0 && id <= 0xffffffff);
  await assert.rejects(workflow.createRemovalOperationId(async () => new Uint8Array(3)));
});

test('un verrou bloque les doubles retraits concurrents', () => {
  const lock = { active: false };
  assert.equal(workflow.acquireRemovalAction(lock), true);
  assert.equal(workflow.acquireRemovalAction(lock), false);
  workflow.releaseRemovalAction(lock);
  assert.equal(workflow.acquireRemovalAction(lock), true);
});

test('retrait impossible si lampe ou MAIN hors ligne, commande incertaine ou opération pendante', () => {
  const ready = { lampOnline: true, mainOnline: true, inventoryFresh: true,
    commandPhase: 'executed', hasPendingRemoval: false, storageAvailable: true,
    actionLocked: false };
  assert.equal(workflow.canRequestRemoval(ready), true);
  for (const patch of [
    { lampOnline: false }, { mainOnline: false }, { inventoryFresh: false },
    { commandPhase: 'uncertain' },
    { hasPendingRemoval: true }, { storageAvailable: false }, { actionLocked: true },
  ]) assert.equal(workflow.canRequestRemoval({ ...ready, ...patch }), false);
});

test('un retrait bloque ON/OFF uniquement sur la lampe cible; pairing reste distinct', () => {
  assert.equal(workflow.isRemovalBlockingCommand(11, 11), true);
  assert.equal(workflow.isRemovalBlockingCommand(12, 11), false);
  assert.equal(workflow.isCommandBlockingRemoval('uncertain'), true);
  assert.equal(workflow.isCommandBlockingRemoval('tracking'), false);
  assert.equal(workflow.isCommandBlockingRemoval('executed'), false);
  assert.match(viewSource, /onPress=\{openPairing\}/);
});

test('suivi processing puis succeeded et retry uniquement par GET', async () => {
  let clock = 0;
  let reads = 0;
  const states = ['processing', 'processing', 'succeeded'];
  const result = await workflow.pollRemovalOperation(1234, async () => {
    reads++;
    return operation(states.shift());
  }, { now: () => clock, sleep: async (ms) => { clock += ms; }, delaysMs: [10] });
  assert.equal(result.kind, 'succeeded');
  assert.equal(reads, 3);
  assert.equal(clock, 20);
});

test('échec confirmé reste séparé d’un retrait réussi', async () => {
  const result = await workflow.pollRemovalOperation(1234,
    async () => operation('failed'));
  assert.equal(result.kind, 'failed');
  assert.equal(result.operation.result_code, 'failed');
});

test('uncertain n’est ni succès ni échec et reste suivi jusqu’au délai local', async () => {
  let clock = 0;
  const result = await workflow.pollRemovalOperation(1234,
    async () => operation('uncertain'), {
      timeoutMs: 25, now: () => clock, sleep: async (ms) => { clock += ms; },
      delaysMs: [10],
    });
  assert.equal(result.kind, 'uncertain');
  assert.equal(result.operation.state, 'uncertain');
});

test('not_found reste incertain et ne devient pas un ordre de renvoi du POST', async () => {
  let reads = 0;
  const result = await workflow.pollRemovalOperation(1234, async () => {
    reads++;
    return operation('not_found', 1234, 0);
  });
  assert.equal(result.kind, 'not_found');
  assert.equal(reads, 1);
  assert.match(viewSource, /Cela ne prouve pas que le POST n’a jamais été reçu/);
  assert.match(viewSource, /Aucun nouveau POST ne sera envoyé/);
});

test('une perte de réseau pendant le POST ne permet qu’un suivi GET au même ID', async () => {
  let posts = 0;
  let gets = 0;
  const first = async () => {
    posts++;
    throw new api.CoreApiError('submission_uncertain', 'response lost');
  };
  let submitted;
  try { await first(); } catch (error) {
    assert.equal(error.kind, 'submission_uncertain');
    submitted = { kind: 'uncertain' };
  }
  assert.equal(submitted.kind, 'uncertain');
  const result = await workflow.pollRemovalOperation(1234, async () => {
    gets++;
    return operation('succeeded');
  });
  assert.equal(result.kind, 'succeeded');
  assert.equal(posts, 1);
  assert.equal(gets, 1);
});

test('reprise après redémarrage lit le même operation_id sans nouvelle soumission', async () => {
  const persisted = { operationId: 1234, deviceId: 11, deviceName: 'Lampe salon',
    coreIp: '192.168.1.20', phase: 'processing' };
  let posts = 0;
  let reads = 0;
  assert.equal(persisted.operationId, 1234);
  const result = await workflow.pollRemovalOperation(persisted.operationId, async () => {
    reads++;
    return operation('succeeded', persisted.operationId, persisted.deviceId);
  });
  assert.equal(result.kind, 'succeeded');
  assert.equal(posts, 0);
  assert.equal(reads, 1);
});

test('une erreur GET temporaire est réessayée en GET, sans second POST', async () => {
  let clock = 0;
  let reads = 0;
  const result = await workflow.pollRemovalOperation(1234, async () => {
    reads++;
    if (reads === 1) throw new api.CoreApiError('network', 'offline');
    return operation('succeeded');
  }, { now: () => clock, sleep: async (ms) => { clock += ms; }, delaysMs: [5] });
  assert.equal(result.kind, 'succeeded');
  assert.equal(reads, 2);
});

test('SecureStore reprend le même retrait et n’y enregistre aucun Bearer token', async () => {
  const values = new Map();
  const secureStore = {
    getItemAsync: async (key) => values.get(key) ?? null,
    setItemAsync: async (key, value) => { values.set(key, value); },
    deleteItemAsync: async (key) => { values.delete(key); },
  };
  const originalLoad = Module._load;
  Module._load = function loadWithSecureStore(request, parent, isMain) {
    if (request === 'expo-secure-store') return secureStore;
    return originalLoad.call(this, request, parent, isMain);
  };
  let store;
  try {
    store = require('../src/features/lighting/removalOperationStore.ts');
  } finally {
    Module._load = originalLoad;
  }
  const pending = { operationId: 1234, deviceId: 11, deviceName: 'Lampe salon',
    coreIp: '192.168.1.20', phase: 'uncertain' };
  await store.savePendingRemoval(pending);
  assert.deepEqual(await store.loadPendingRemoval(), pending);
  assert.equal([...values.values()].some((value) => value.includes('Bearer')), false);
  assert.equal([...values.keys()].length, 1);
  await store.clearPendingRemoval();
  assert.equal(await store.loadPendingRemoval(), null);
});

test('succès déclenche une relecture inventory et le code attend la disparition avant retour liste', () => {
  assert.match(viewSource, /const inventory = await refreshRemovalInventory\(operation\.deviceId\)/);
  assert.match(viewSource, /if \(inventory === 'missing'\)[\s\S]*setSelectedId\(null\)/);
  assert.match(viewSource, /if \(inventory === 'present'\)[\s\S]*ne recommencez pas le retrait/);
});

test('résultat succeeded mais inventaire encore présent ne renvoie pas le retrait', () => {
  assert.match(viewSource, /ne recommencez pas le retrait/);
  assert.match(viewSource, /Actualiser l’inventaire/);
  assert.match(viewSource, /operation\.phase === 'succeeded'[\s\S]*finishRemovalSuccess\(operation\)/);
});

test('état inconnu, appairage V7.6 et API de commande restent présents', () => {
  assert.match(viewSource, /Dernier état confirmé/);
  assert.match(viewSource, /<PairingScreen ip=\{ip\} token=\{token\}/);
  assert.match(viewSource, /onPower=\{\(state\) => void requestPower\(lamp\.id, state\)\}/);
  assert.match(viewSource, /requestDeviceRemoval\(ip, lamp\.id, operationId, token\)/);
});

