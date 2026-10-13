const assert = require('node:assert/strict');
const fs = require('node:fs');
const { test, afterEach } = require('node:test');
const ts = require('typescript');

require.extensions['.ts'] = (module, filename) => {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  module._compile(output, filename);
};

const api = require('../src/api/coreApiClient.ts');
const workflow = require('../src/features/pairing/pairingWorkflow.ts');
const originalFetch = global.fetch;
const token = 'test-token-not-a-real-secret-1234567890';
const candidate = {
  hardware_id: 'C6-ACEBE6FFFE0C8E90', capabilities: 3, last_seen_ms: 1234,
};
const activeSession = { id: 502669618, state: 'active', duration_seconds: 60,
  candidate_count: 1, candidate_limit_reached: false };

afterEach(() => { global.fetch = originalFetch; });

function respond(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status, headers: { 'Content-Type': 'application/json' },
  });
}

test('ouverture session envoie une seule requête authentifiée avec la durée permise', async () => {
  const calls = [];
  global.fetch = async (url, options) => {
    calls.push({ url, options });
    return respond({ session: { id: activeSession.id, state: 'opening', duration_seconds: 60 } }, 202);
  };
  const result = await api.startPairingSession('192.168.1.20', 60, token);
  assert.equal(result.session.state, 'opening');
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, 'http://192.168.1.20/api/v1/pairing/session');
  assert.equal(calls[0].options.method, 'POST');
  assert.equal(calls[0].options.headers.Authorization, `Bearer ${token}`);
  assert.deepEqual(JSON.parse(calls[0].options.body), { duration_seconds: 60 });
});

test('GET session lit null ou reprend la session active avec authentification', async () => {
  const calls = [];
  const sessions = [null, activeSession];
  global.fetch = async (url, options) => {
    calls.push([url, options.method, options.headers.Authorization]);
    return respond({ session: sessions.shift() });
  };
  assert.equal((await api.getPairingSession('192.168.1.20', token)).session, null);
  assert.equal((await api.getPairingSession('192.168.1.20', token)).session.id, activeSession.id);
  assert.ok(calls.every(([url, method, auth]) =>
    url.endsWith('/api/v1/pairing/session') && method === 'GET' && auth === `Bearer ${token}`));
});

test('candidates accepte une liste vide et identifie les candidates par hardware_id', async () => {
  const bodies = [
    { session_id: activeSession.id, capacity_reached: false, candidates: [], count: 0 },
    { session_id: activeSession.id, capacity_reached: false, candidates: [candidate], count: 1 },
    { session_id: activeSession.id, capacity_reached: true, candidates: [candidate], count: 1 },
  ];
  global.fetch = async () => respond(bodies.shift());
  assert.equal((await api.getPairingCandidates('192.168.1.20', token)).count, 0);
  const discovered = await api.getPairingCandidates('192.168.1.20', token);
  assert.equal(discovered.candidates[0].hardware_id, candidate.hardware_id);
  assert.equal(workflow.findPairingCandidate(discovered.candidates, candidate.hardware_id),
    discovered.candidates[0]);
  assert.equal(workflow.findPairingCandidate(discovered.candidates, 'C6-FFFFFFFFFFFFFFFF'), null);
  assert.equal(workflow.retainPairingSelection([], candidate.hardware_id), null);
  assert.equal((await api.getPairingCandidates('192.168.1.20', token)).capacity_reached, true);
});

test('création operation transmet seulement la candidate explicitement sélectionnée', async () => {
  let requestBody;
  global.fetch = async (url, options) => {
    requestBody = JSON.parse(options.body);
    assert.equal(url, 'http://192.168.1.20/api/v1/pairing/operations');
    assert.equal(options.headers.Authorization, `Bearer ${token}`);
    return respond({ operation: { id: 987654321, session_id: activeSession.id,
      hardware_id: candidate.hardware_id, state: 'pending' } }, 202);
  };
  const request = { session_id: activeSession.id, operation_id: 987654321,
    hardware_id: candidate.hardware_id, name: 'Lampe salon' };
  const response = await api.requestPairingOperation('192.168.1.20', request, token);
  assert.equal(response.operation.id, request.operation_id);
  assert.deepEqual(requestBody, request);
});

test('noms et identifiants hardware respectent les limites réelles du firmware', () => {
  assert.equal(api.isValidPairingHardwareId(candidate.hardware_id), true);
  assert.equal(api.isValidPairingHardwareId('C6-ABC'), false);
  assert.equal(workflow.isValidPairingName('Lampe salon-1'), true);
  assert.equal(workflow.isValidPairingName(''), false);
  assert.equal(workflow.isValidPairingName('lampe/1'), false);
  assert.equal(workflow.isValidPairingName('x'.repeat(32)), false);
});

test('un ID operation est non nul sur 32 bits et ne se répète pas dans le processus', () => {
  const first = workflow.createPairingOperationId(() => 123, () => 0);
  const second = workflow.createPairingOperationId(() => 123, () => 0);
  assert.ok(first > 0 && first <= 0xffffffff);
  assert.ok(second > 0 && second <= 0xffffffff);
  assert.notEqual(first, second);
});

test('ouverture attend l’état active sans refaire le POST', async () => {
  let posts = 0;
  let clock = 0;
  const states = [
    { session: { ...activeSession, state: 'opening' } },
    { session: activeSession },
  ];
  const result = await workflow.openPairingSessionOnce(async () => {
    posts++;
    return states.shift();
  }, async () => states.shift(), {
    now: () => clock,
    sleep: async (ms) => { clock += ms; },
    intervalMs: 5,
  });
  assert.equal(result.kind, 'ready');
  assert.equal(result.value.state, 'active');
  assert.equal(posts, 1);
});

test('réponse perdue à l’ouverture est réconciliée par GET sans second POST', async () => {
  let posts = 0;
  const result = await workflow.openPairingSessionOnce(async () => {
    posts++;
    throw new api.CoreApiError('submission_uncertain', 'lost');
  }, async () => ({ session: activeSession }));
  assert.equal(result.kind, 'ready');
  assert.equal(posts, 1);
});

test('session absente après ouverture incertaine ne provoque pas de nouvelle session', async () => {
  let posts = 0;
  const result = await workflow.openPairingSessionOnce(async () => {
    posts++;
    throw new api.CoreApiError('submission_uncertain', 'lost');
  }, async () => ({ session: null }));
  assert.equal(result.kind, 'none');
  assert.equal(posts, 1);
});

test('opération suit pending jusqu’à succeeded', async () => {
  let clock = 0;
  const states = ['pending', 'processing', 'succeeded'];
  const result = await workflow.pollPairingOperation(987654321, async () => ({ operation: {
    id: 987654321, session_id: activeSession.id, hardware_id: candidate.hardware_id,
    name: 'Lampe salon', state: states.shift(), result_code: 'succeeded', local_device_id: 11,
  } }), { now: () => clock, sleep: async (ms) => { clock += ms; }, delaysMs: [5] });
  assert.equal(result.kind, 'ready');
  assert.equal(result.value.state, 'succeeded');
});

test('failed, expired et uncertain restent des états terminaux distincts', async () => {
  for (const state of ['failed', 'expired', 'uncertain']) {
    const result = await workflow.pollPairingOperation(987654321, async () => ({ operation: {
      id: 987654321, session_id: activeSession.id, hardware_id: candidate.hardware_id,
      name: 'Lampe salon', state, result_code: state, local_device_id: 0,
    } }));
    assert.equal(result.kind, 'ready');
    assert.equal(result.value.state, state);
  }
});

test('perte de réponse du POST retrouve l’opération par ID sans doubler le POST', async () => {
  let posts = 0;
  let reads = 0;
  const request = { session_id: activeSession.id, operation_id: 987654321,
    hardware_id: candidate.hardware_id, name: 'Lampe salon' };
  const recovered = { id: request.operation_id, session_id: request.session_id,
    hardware_id: candidate.hardware_id, name: request.name, state: 'processing',
    result_code: 'pending', local_device_id: 0 };
  const result = await workflow.submitPairingOperationOnce(request, async () => {
    posts++;
    throw new api.CoreApiError('submission_uncertain', 'lost');
  }, async () => { reads++; return { operation: recovered }; });
  assert.equal(result.kind, 'recovered');
  assert.equal(result.operation.id, request.operation_id);
  assert.equal(posts, 1);
  assert.equal(reads, 1);
});

test('opération introuvable après POST perdu reste incertaine et ne rejoue pas le POST', async () => {
  let posts = 0;
  let reads = 0;
  const request = { session_id: activeSession.id, operation_id: 987654322,
    hardware_id: candidate.hardware_id, name: 'Lampe salon' };
  const result = await workflow.submitPairingOperationOnce(request, async () => {
    posts++;
    throw new api.CoreApiError('submission_uncertain', 'lost');
  }, async () => {
    reads++;
    throw new api.CoreApiError('http', 'missing', 404, 'operation_not_found');
  }, { sleep: async () => {} });
  assert.equal(result.kind, 'uncertain');
  assert.equal(posts, 1);
  assert.equal(reads, 3);
});

test('fermeture DELETE attend la confirmation GET sans seconde fermeture', async () => {
  let deletes = 0;
  let reads = 0;
  let clock = 0;
  const result = await workflow.closePairingSessionOnce(async () => {
    deletes++;
    return { session: { id: activeSession.id, state: 'closing' } };
  }, async () => {
    reads++;
    return { session: reads === 1 ? { ...activeSession, state: 'closing' } :
      { ...activeSession, state: 'closed' } };
  }, { now: () => clock, sleep: async (ms) => { clock += ms; }, intervalMs: 5 });
  assert.equal(result.kind, 'ready');
  assert.equal(result.value.state, 'closed');
  assert.equal(deletes, 1);
  assert.equal(reads, 2);
});

test('toutes les routes pairing sont authentifiées et fermeture accepte la réponse asynchrone', async () => {
  const requests = [];
  global.fetch = async (url, options) => {
    requests.push([url, options.method, options.headers.Authorization]);
    if (url.endsWith('/pairing/operations') && options.method === 'POST') return respond({ operation: {
      id: 987654321, session_id: activeSession.id, hardware_id: candidate.hardware_id,
      state: 'pending',
    } }, 202);
    if (url.endsWith('/pairing/session') && options.method === 'POST') return respond({ session: {
      id: activeSession.id, state: 'opening', duration_seconds: 60,
    } }, 202);
    if (options.method === 'DELETE') return respond({ session: {
      id: activeSession.id, state: 'closing',
    } }, 202);
    if (url.endsWith('/candidates')) return respond({ session_id: activeSession.id,
      capacity_reached: false, candidates: [candidate], count: 1 });
    if (url.endsWith('/987654321')) return respond({ operation: {
      id: 987654321, session_id: activeSession.id, hardware_id: candidate.hardware_id,
      name: 'Lampe salon', state: 'succeeded', result_code: 'succeeded', local_device_id: 11,
    } });
    return respond({ session: activeSession });
  };
  await api.startPairingSession('192.168.1.20', 60, token);
  await api.getPairingSession('192.168.1.20', token);
  await api.getPairingCandidates('192.168.1.20', token);
  await api.requestPairingOperation('192.168.1.20', { session_id: activeSession.id,
    operation_id: 987654321, hardware_id: candidate.hardware_id, name: 'Lampe salon' }, token);
  await api.getPairingOperation('192.168.1.20', 987654321, token);
  await api.closePairingSession('192.168.1.20', token);
  assert.equal(requests.length, 6);
  assert.ok(requests.every(([, , authorization]) => authorization === `Bearer ${token}`));
  assert.deepEqual(requests.map(([, method]) => method), ['POST', 'GET', 'GET', 'POST', 'GET', 'DELETE']);
});

test('authentification refusée reste une erreur explicite', async () => {
  global.fetch = async () => respond({ error: { code: 'unauthorized', message: 'denied' } }, 401);
  await assert.rejects(api.getPairingSession('192.168.1.20', token), (error) =>
    error instanceof api.CoreApiError && error.status === 401 &&
    api.describeApiError(error).includes('jeton'));
});

test('reprise au premier plan attend l’activation avant le GET de session', async () => {
  let activeWaits = 0;
  let reads = 0;
  let clock = 0;
  const states = [
    { session: { ...activeSession, state: 'opening' } },
    { session: activeSession },
  ];
  const result = await workflow.pollPairingSession(async () => {
    reads++;
    return states.shift();
  }, 'active', { now: () => clock, sleep: async (ms) => { clock += ms; },
    intervalMs: 5, waitUntilActive: async () => { activeWaits++; } });
  assert.equal(result.kind, 'ready');
  assert.equal(reads, 2);
  assert.equal(activeWaits, 2);
});

test('verrou empêche plusieurs sessions ou opérations concurrentes dans l’interface', () => {
  const lock = { active: false };
  assert.equal(workflow.acquirePairingAction(lock), true);
  assert.equal(workflow.acquirePairingAction(lock), false);
  workflow.releasePairingAction(lock);
  assert.equal(workflow.acquirePairingAction(lock), true);
  workflow.releasePairingAction(lock);
});

test('inventaire post-succès distingue une lampe nouvellement ajoutée', async () => {
  const devices = [
    { id: 101, name: 'lampe existante', role: 'lamp', online: true, status: 'online',
      parent_id: 7, capabilities: [], last_seen_ms: 1, state: null,
      last_confirmed_state: { power: null, status: 'unknown' } },
    { id: 102, name: 'Lampe salon', role: 'lamp', online: true, status: 'online',
      parent_id: 7, capabilities: [], last_seen_ms: 2, state: null,
      last_confirmed_state: { power: null, status: 'unknown' } },
  ];
  const before = new Set([101]);
  assert.equal(workflow.findNewlyPairedLamp(devices, before, 'Lampe salon').id, 102);
  assert.equal(workflow.findNewlyPairedLamp(devices, before, 'missing'), undefined);
});
