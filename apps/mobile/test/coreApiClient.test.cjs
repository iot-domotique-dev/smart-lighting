const assert = require('node:assert/strict');
const fs = require('node:fs');
const { test, afterEach } = require('node:test');
const ts = require('typescript');

// Exécute le client TypeScript avec des réponses HTTP simulées, sans runtime mobile.
require.extensions['.ts'] = (module, filename) => {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  module._compile(output, filename);
};

const { CoreApiError, describeApiError, getHealth, getCore, getModules, getDevices,
  getDevice, getCommand, getRemovalOperation, lampsOnly, requestDeviceRemoval,
  submitBrightness, submitPower, getGroups, getGroup, createGroup, renameGroup,
  changeGroupMember, deleteGroup, getGroupOperation, setGroupPower } =
  require('../src/api/coreApiClient.ts');
const { acquireLampCommand, pollCommand, releaseLampCommand } =
  require('../src/features/lighting/commandPolling.ts');
const { groupMemberResultLabel } = require('../src/features/groups/groupOperationLabels.ts');
const originalFetch = global.fetch;

afterEach(() => { global.fetch = originalFetch; });

const base = {
  name: 'lamp001', role: 'lamp', online: true, status: 'online',
  parent_id: 4074601247, capabilities: [], last_seen_ms: 1000, state: null,
};

function respond(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status, headers: { 'Content-Type': 'application/json' },
  });
}

test('lecture health/core et inventaire de deux lampes indépendantes', async () => {
  const requests = [];
  global.fetch = async (url, options) => {
    requests.push([url, options.method]);
    if (url.endsWith('/health')) return respond({ status: 'ok', core_id: 1,
      firmware_version: 'v7.4', uptime_ms: 12000, wifi_connected: true,
      uart_driver_ready: true });
    if (url.endsWith('/core')) return respond({ core: { id: 1, name: 'CORE',
      role: 'core', status: 'online', online: true, api_version: 'v1',
      firmware_version: 'v7.4', uptime_ms: 12000, wifi_connected: true,
      uart_driver_ready: true } });
    if (url.endsWith('/modules')) return respond({ modules: [{ id: 4074601247,
      name: 'MAIN_LIGHTING', role: 'main', online: true, status: 'online',
      parent_id: 1, capabilities: ['lighting'], last_seen_ms: 1000, state: null }],
    count: 1 });
    if (url.endsWith('/devices')) return respond({ devices: [
      { ...base, id: 11, capabilities: ['power', 'brightness'],
        last_confirmed_state: { power: 'on', status: 'confirmed',
          brightness_percent: 50, brightness_status: 'confirmed' } },
      { ...base, id: 12, name: 'lamp002', online: false, status: 'offline',
        capabilities: ['power', 'brightness'],
        last_confirmed_state: { power: 'off', status: 'stale',
          brightness_percent: 25, brightness_status: 'stale' } },
      { ...base, id: 13, role: 'sensor', last_confirmed_state: null },
    ], count: 3 });
    if (url.endsWith('/devices/12')) return respond({ device: { ...base, id: 12,
      name: 'lamp002', online: false, status: 'offline',
      last_confirmed_state: { power: 'off', status: 'stale' } } });
    throw new Error('unexpected route');
  };

  assert.equal((await getHealth('192.168.1.20')).core_id, 1);
  assert.equal((await getCore('192.168.1.20')).core.id, 1);
  assert.equal((await getModules('192.168.1.20')).modules[0].id, 4074601247);
  const lamps = lampsOnly((await getDevices('192.168.1.20')).devices);
  assert.deepEqual(lamps.map((lamp) => lamp.id), [11, 12]);
  assert.deepEqual(lamps.map((lamp) => lamp.last_confirmed_state.status),
    ['confirmed', 'stale']);
  assert.equal(lamps[0].last_confirmed_state.brightness_percent, 50);
  assert.equal(lamps[1].last_confirmed_state.brightness_status, 'stale');
  assert.equal((await getDevice('192.168.1.20', 12)).device.id, 12);
  assert.ok(requests.every(([url, method]) =>
    url.startsWith('http://192.168.1.20/api/v1/') && method === 'GET'));
});

test('une réponse d’état lampe invalide est refusée', async () => {
  global.fetch = async () => respond({ devices: [{ ...base, id: 11,
    last_confirmed_state: { power: 'on', status: 'unknown' } }], count: 1 });
  await assert.rejects(getDevices('192.168.1.20'),
    (error) => error instanceof CoreApiError && error.kind === 'invalid_response');
});

test('le client groupe utilise les routes MAIN, Bearer et les IDs V7', async () => {
  const mainId = 4074601247;
  const groupId = 901;
  const operationId = 902;
  const group = { id: groupId, main_id: mainId, name: 'Salon', revision: 2,
    member_ids: [3559985816] };
  const requests = [];
  global.fetch = async (url, options) => {
    requests.push({ url, options });
    if (url.endsWith(`/modules/${mainId}/groups`) && options.method === 'GET')
      return respond({ groups: [group], count: 1 });
    if (url.endsWith(`/modules/${mainId}/groups/${groupId}`) && options.method === 'GET')
      return respond({ group });
    if (options.method === 'GET')
      return respond({ operation: { id: operationId, state: 'persisted',
        result: 'persisted', group_id: groupId, revision: 3 } });
    return respond({ operation: { id: operationId, state: 'sent' } }, 202);
  };

  assert.deepEqual((await getGroups('192.168.1.20', mainId, 'test-token')).groups, [group]);
  assert.deepEqual((await getGroup('192.168.1.20', mainId, groupId, 'test-token')).group, group);
  assert.equal((await createGroup('192.168.1.20', mainId,
    { operation_id: operationId, name: 'Bureau' }, 'test-token')).operation.state, 'sent');
  await renameGroup('192.168.1.20', mainId, groupId,
    { operation_id: operationId, expected_revision: 2, name: 'Salon 2' }, 'test-token');
  await changeGroupMember('192.168.1.20', mainId, groupId,
    { operation_id: operationId, expected_revision: 2,
      member_action: 'remove', lamp_id: 3559985816 }, 'test-token');
  await deleteGroup('192.168.1.20', mainId, groupId,
    { operation_id: operationId, expected_revision: 2 }, 'test-token');
  assert.equal((await getGroupOperation('192.168.1.20', operationId, 'test-token'))
    .operation.state, 'persisted');

  assert.deepEqual(requests.map(({ url, options }) => [new URL(url).pathname, options.method]), [
    [`/api/v1/modules/${mainId}/groups`, 'GET'],
    [`/api/v1/modules/${mainId}/groups/${groupId}`, 'GET'],
    [`/api/v1/modules/${mainId}/groups`, 'POST'],
    [`/api/v1/modules/${mainId}/groups/${groupId}`, 'PATCH'],
    [`/api/v1/modules/${mainId}/groups/${groupId}`, 'PATCH'],
    [`/api/v1/modules/${mainId}/groups/${groupId}`, 'DELETE'],
    [`/api/v1/group-operations/${operationId}`, 'GET'],
  ]);
  assert.ok(requests.every(({ options }) =>
    options.headers.Authorization === 'Bearer test-token'));
  assert.equal(JSON.parse(requests[2].options.body).operation_id, operationId);
  assert.equal(JSON.parse(requests[4].options.body).lamp_id, 3559985816);
});

test('une réponse HTTP perdue sur une mutation de groupe n’est jamais renvoyée automatiquement', async () => {
  let calls = 0;
  global.fetch = async () => { calls++; throw new Error('connection reset'); };
  await assert.rejects(createGroup('192.168.1.20', 4074601247,
    { operation_id: 123456, name: 'Entree' }, 'test-token'),
  (error) => error instanceof CoreApiError && error.kind === 'submission_uncertain');
  assert.equal(calls, 1);
});

test('ON collectif envoie une seule commande parent authentifiée et les résultats gardent chaque lampe', async () => {
  const requests = [];
  global.fetch = async (url, options) => {
    requests.push({ url, options });
    if (options.method === 'POST')
      return respond({ operation: { id: 912345, state: 'sent' } }, 202);
    return respond({ operation: { id: 912345, state: 'completed', result: 'partial',
      group_id: 9001, members: [
        { lamp_id: 3559985816, child_id: 5001, state: 'executed', result: 'executed' },
        { lamp_id: 3559985817, child_id: 5002, state: 'offline', result: 'offline' },
      ] } });
  };
  const submitted = await setGroupPower('192.168.1.20', 4074601247, 9001,
    { operation_id: 912345, power: 'on' }, 'group-test-token');
  const status = await getGroupOperation('192.168.1.20', 912345, 'group-test-token');
  assert.equal(submitted.operation.state, 'sent');
  assert.equal(status.operation.result, 'partial');
  assert.deepEqual(status.operation.members.map((member) => member.state),
    ['executed', 'offline']);
  assert.equal(requests.length, 2);
  assert.equal(new URL(requests[0].url).pathname,
    '/api/v1/modules/4074601247/groups/9001/commands/power');
  assert.equal(requests[0].options.method, 'POST');
  assert.deepEqual(JSON.parse(requests[0].options.body),
    { operation_id: 912345, power: 'on' });
  assert.equal(new URL(requests[1].url).pathname,
    '/api/v1/group-operations/912345');
  assert.ok(requests.every(({ options }) =>
    options.headers.Authorization === 'Bearer group-test-token'));
});

test('réponse POST collective perdue est incertaine et le client ne répète jamais le POST', async () => {
  let calls = 0;
  global.fetch = async () => { calls++; throw new Error('connection reset'); };
  await assert.rejects(setGroupPower('192.168.1.20', 4074601247, 9001,
    { operation_id: 912346, power: 'off' }, 'group-test-token'),
  (error) => error instanceof CoreApiError && error.kind === 'submission_uncertain');
  assert.equal(calls, 1);
});

test('réponse collective refuse les doublons de lampe ou d’identifiant enfant', async () => {
  for (const members of [
    [
      { lamp_id: 11, child_id: 50, state: 'executed', result: 'executed' },
      { lamp_id: 11, child_id: 51, state: 'offline', result: 'offline' },
    ],
    [
      { lamp_id: 11, child_id: 50, state: 'executed', result: 'executed' },
      { lamp_id: 12, child_id: 50, state: 'uncertain', result: 'superseded' },
    ],
  ]) {
    global.fetch = async () => respond({ operation: { id: 912347,
      state: 'completed', result: 'partial', members } });
    await assert.rejects(getGroupOperation('192.168.1.20', 912347, 'group-test-token'),
      (error) => error instanceof CoreApiError && error.kind === 'invalid_response');
  }
});

test('l’écran traduit les résultats par lampe et signale OFF prioritaire', () => {
  assert.equal(groupMemberResultLabel({ lamp_id: 11, child_id: 50,
    state: 'executed', result: 'executed' }), 'exécutée et confirmée');
  assert.equal(groupMemberResultLabel({ lamp_id: 12, child_id: 51,
    state: 'offline', result: 'offline' }), 'hors ligne');
  assert.equal(groupMemberResultLabel({ lamp_id: 13, child_id: 52,
    state: 'uncertain', result: 'superseded' }),
  'résultat incertain · remplacée par OFF prioritaire');
});

test('le client accepte les états brightness unknown et les réponses V7.8 sans champs brightness', async () => {
  global.fetch = async (url) => {
    const state = url.endsWith('/devices/11')
      ? { power: null, status: 'unknown', brightness_percent: null,
        brightness_status: 'unknown' }
      : { power: 'off', status: 'confirmed' };
    return respond({ device: { ...base, id: url.endsWith('/devices/11') ? 11 : 12,
      last_confirmed_state: state } });
  };
  assert.equal((await getDevice('192.168.1.20', 11)).device.last_confirmed_state.brightness_status,
    'unknown');
  assert.equal((await getDevice('192.168.1.20', 12)).device.last_confirmed_state.brightness_percent,
    undefined);
});

test('le client refuse les états brightness incohérents ou hors plage', async () => {
  for (const last_confirmed_state of [
    { power: 'on', status: 'confirmed', brightness_percent: null,
      brightness_status: 'confirmed' },
    { power: 'on', status: 'confirmed', brightness_percent: 101,
      brightness_status: 'confirmed' },
    { power: 'on', status: 'confirmed', brightness_percent: 20 },
  ]) {
    global.fetch = async () => respond({ devices: [
      { ...base, id: 11, last_confirmed_state },
    ], count: 1 });
    await assert.rejects(getDevices('192.168.1.20'),
      (error) => error instanceof CoreApiError && error.kind === 'invalid_response');
  }
});

test('erreur HTTP du CORE et panne réseau sont distinguées', async () => {
  global.fetch = async () => respond({ error: { code: 'core_not_ready',
    message: 'The CORE registry is not ready.' } }, 503);
  await assert.rejects(getHealth('192.168.1.20'),
    (error) => error instanceof CoreApiError && error.kind === 'http' &&
      error.status === 503 && error.code === 'core_not_ready');
  global.fetch = async () => { throw new Error('network down'); };
  await assert.rejects(getHealth('192.168.1.20'),
    (error) => error instanceof CoreApiError && error.kind === 'network');
});

test('POST ON et OFF utilisent la route existante et le Bearer token', async () => {
  const requests = [];
  let nextId = 201;
  global.fetch = async (url, options) => {
    requests.push({ url, options });
    return respond({ command: { id: nextId++, state: 'sent' } }, 202);
  };
  const token = 'test-token-not-a-real-secret-1234567890';

  const on = await submitPower('192.168.1.20', 11, 'on', token);
  const off = await submitPower('192.168.1.20', 12, 'off', token);

  assert.deepEqual(on.command, { id: 201, state: 'sent' });
  assert.deepEqual(off.command, { id: 202, state: 'sent' });
  assert.deepEqual(requests.map(({ url, options }) => [url, options.method,
    options.headers.Authorization, JSON.parse(options.body).state]), [
    ['http://192.168.1.20/api/v1/devices/11/commands/power', 'POST', `Bearer ${token}`, 'on'],
    ['http://192.168.1.20/api/v1/devices/12/commands/power', 'POST', `Bearer ${token}`, 'off'],
  ]);
});

test('POST brightness envoie les entiers 0, 1, 50 et 100 sur la route V7.8.2', async () => {
  const requests = [];
  let nextId = 310;
  global.fetch = async (url, options) => {
    requests.push({ url, options });
    return respond({ command: { id: nextId++, state: 'sent' } }, 202);
  };
  const token = 'test-token-not-a-real-secret-1234567890';
  for (const percent of [0, 1, 50, 100]) {
    const response = await submitBrightness('192.168.1.20', 11, percent, token);
    assert.equal(response.command.state, 'sent');
  }
  assert.deepEqual(requests.map(({ url, options }) => [url, options.method,
    options.headers.Authorization, JSON.parse(options.body)]), [
    ['http://192.168.1.20/api/v1/devices/11/commands/brightness', 'POST',
      `Bearer ${token}`, { percent: 0 }],
    ['http://192.168.1.20/api/v1/devices/11/commands/brightness', 'POST',
      `Bearer ${token}`, { percent: 1 }],
    ['http://192.168.1.20/api/v1/devices/11/commands/brightness', 'POST',
      `Bearer ${token}`, { percent: 50 }],
    ['http://192.168.1.20/api/v1/devices/11/commands/brightness', 'POST',
      `Bearer ${token}`, { percent: 100 }],
  ]);
});

test('la validation brightness bloque les valeurs non entières/hors plage sans requête', async () => {
  let attempts = 0;
  global.fetch = async () => { attempts++; return respond({}); };
  for (const percent of [-1, 101, 1.5, NaN, Infinity]) {
    await assert.rejects(submitBrightness('192.168.1.20', 11, percent,
      'test-token-123456789012345678901234'),
    (error) => error instanceof CoreApiError && error.kind === 'invalid_response');
  }
  assert.equal(attempts, 0);
});

test('une réponse POST brightness perdue est marquée incertaine et ne peut être renvoyée par le client', async () => {
  let attempts = 0;
  global.fetch = async () => { attempts++; throw new Error('response lost'); };
  await assert.rejects(submitBrightness('192.168.1.20', 11, 50,
    'test-token-123456789012345678901234'),
  (error) => error instanceof CoreApiError && error.kind === 'submission_uncertain');
  assert.equal(attempts, 1);
});

test('brightness suit sent → accepted → executed puis relit le niveau confirmé', async () => {
  const requests = [];
  const states = ['sent', 'accepted', 'executed'];
  global.fetch = async (url, options) => {
    requests.push({ url, options });
    if (url.endsWith('/commands/901')) {
      return respond({ command: { id: 901, state: states.shift(), retries: 0 } });
    }
    if (url.endsWith('/devices/11')) return respond({ device: {
      ...base, id: 11, capabilities: ['power', 'brightness'],
      last_confirmed_state: { power: 'on', status: 'confirmed',
        brightness_percent: 50, brightness_status: 'confirmed' },
    } });
    return respond({ command: { id: 901, state: 'sent' } }, 202);
  };
  const token = 'test-token-not-a-real-secret-1234567890';
  const submitted = await submitBrightness('192.168.1.20', 11, 50, token);
  const progress = [];
  const outcome = await pollCommand(submitted.command.id,
    () => getCommand('192.168.1.20', submitted.command.id, token), {
      sleep: async () => {}, onStatus: (command) => progress.push(command.state),
    });
  assert.deepEqual(progress, ['sent', 'accepted', 'executed']);
  assert.equal(outcome.kind, 'terminal');
  const detail = await getDevice('192.168.1.20', 11);
  assert.deepEqual(detail.device.last_confirmed_state, { power: 'on', status: 'confirmed',
    brightness_percent: 50, brightness_status: 'confirmed' });
  assert.deepEqual(requests.map(({ url, options }) => options.method),
    ['POST', 'GET', 'GET', 'GET', 'GET']);
  assert.equal(requests.filter(({ options }) => options.method === 'POST').length, 1);
});

test('POST retrait envoie une seule requête avec ID V7, operation_id et Bearer', async () => {
  const requests = [];
  global.fetch = async (url, options) => {
    requests.push({ url, options });
    return respond({ operation: { id: 7001, device_id: 3559985816,
      state: 'processing' } }, 202);
  };
  const response = await requestDeviceRemoval('192.168.1.20', 3559985816, 7001,
    'test-token-123456789012345678901234');
  assert.deepEqual(response.operation, { id: 7001, device_id: 3559985816,
    state: 'processing' });
  assert.equal(requests.length, 1);
  assert.equal(requests[0].url,
    'http://192.168.1.20/api/v1/devices/3559985816/removal-operations');
  assert.equal(requests[0].options.method, 'POST');
  assert.equal(requests[0].options.headers.Authorization,
    'Bearer test-token-123456789012345678901234');
  assert.deepEqual(JSON.parse(requests[0].options.body), { operation_id: 7001 });
});

test('POST retrait 503 uncertain est suivi sans relancer le POST', async () => {
  let posts = 0;
  global.fetch = async () => {
    posts++;
    return respond({ operation: { id: 7002, device_id: 3559985816,
      state: 'uncertain' } }, 503);
  };
  const response = await requestDeviceRemoval('192.168.1.20', 3559985816, 7002,
    'test-token-123456789012345678901234');
  assert.equal(response.operation.state, 'uncertain');
  assert.equal(posts, 1);
});

test('GET retrait valide processing/succeeded/failed/uncertain/not_found', async () => {
  const requests = [];
  const states = ['processing', 'succeeded', 'failed', 'uncertain', 'not_found'];
  global.fetch = async (url, options) => {
    requests.push([url, options.method, options.headers.Authorization]);
    const state = states.shift();
    return respond({ operation: { id: 7003, device_id: state === 'not_found' ? 0 : 3559985816,
      state, result_code: state } });
  };
  for (const state of ['processing', 'succeeded', 'failed', 'uncertain', 'not_found']) {
    const response = await getRemovalOperation('192.168.1.20', 7003, 3559985816,
      'test-token-123456789012345678901234');
    assert.equal(response.operation.state, state);
  }
  assert.equal(requests.length, 5);
  assert.ok(requests.every(([url, method, authorization]) =>
    url.endsWith('/api/v1/removal-operations/7003') && method === 'GET' &&
    authorization === 'Bearer test-token-123456789012345678901234'));
});

test('GET retrait refuse un résultat associé à une autre lampe', async () => {
  global.fetch = async () => respond({ operation: { id: 7004, device_id: 12,
    state: 'succeeded', result_code: 'succeeded' } });
  await assert.rejects(getRemovalOperation('192.168.1.20', 7004, 11,
    'test-token-123456789012345678901234'),
  (error) => error instanceof CoreApiError && error.kind === 'invalid_response');
});

test('rejet HTTP offline et Bearer invalide restent explicites', async () => {
  global.fetch = async () => respond({ error: { code: 'offline', message: 'offline' } }, 409);
  await assert.rejects(requestDeviceRemoval('192.168.1.20', 11, 7005,
    'test-token-123456789012345678901234'),
  (error) => error instanceof CoreApiError && error.status === 409 &&
    describeApiError(error).includes('hors ligne'));
  global.fetch = async () => respond({ error: { code: 'unauthorized', message: 'denied' } }, 401);
  await assert.rejects(requestDeviceRemoval('192.168.1.20', 11, 7005,
    'test-token-123456789012345678901234'),
  (error) => error instanceof CoreApiError && error.status === 401 &&
    describeApiError(error).includes('jeton'));
});

test('GET commande est authentifié et valide le suivi sent/accepted/executed', async () => {
  const requests = [];
  const states = ['sent', 'accepted', 'executed'];
  global.fetch = async (url, options) => {
    requests.push([url, options.method, options.headers.Authorization]);
    return respond({ command: { id: 303, state: states.shift(), retries: 0 } });
  };
  const token = 'test-token-not-a-real-secret-1234567890';
  const results = await Promise.all([
    getCommand('192.168.1.20', 303, token),
    getCommand('192.168.1.20', 303, token),
    getCommand('192.168.1.20', 303, token),
  ]);
  assert.deepEqual(results.map((result) => result.command.state), ['sent', 'accepted', 'executed']);
  assert.ok(requests.every(([url, method, auth]) =>
    url.endsWith('/api/v1/commands/303') && method === 'GET' && auth === `Bearer ${token}`));
});

test('une perte de réponse POST est incertaine et ne déclenche aucun second POST', async () => {
  let attempts = 0;
  global.fetch = async () => { attempts++; throw new Error('response lost'); };
  await assert.rejects(submitPower('192.168.1.20', 11, 'on', 'test-token-123456789012345678901234'),
    (error) => error instanceof CoreApiError && error.kind === 'submission_uncertain');
  assert.equal(attempts, 1);
});

test('les erreurs API unauthorized et offline sont traduites sans révéler le jeton', async () => {
  global.fetch = async () => respond({ error: { code: 'unauthorized', message: 'denied' } }, 401);
  await assert.rejects(submitPower('192.168.1.20', 11, 'on', 'test-token-123456789012345678901234'),
    (error) => error instanceof CoreApiError && error.status === 401 &&
      describeApiError(error).includes('Vérifiez le jeton') &&
      !describeApiError(error).includes('test-token'));
  global.fetch = async () => respond({ error: { code: 'offline', message: 'offline' } }, 409);
  await assert.rejects(submitPower('192.168.1.20', 11, 'off', 'test-token-123456789012345678901234'),
    (error) => error instanceof CoreApiError && error.status === 409 &&
      describeApiError(error).includes('hors ligne'));
});

test('404 de destination et 503 de commande sont expliquées selon leur code API', async () => {
  global.fetch = async () => respond({ error: {
    code: 'unknown_destination', message: 'missing lamp',
  } }, 404);
  await assert.rejects(submitPower('192.168.1.20', 11, 'on', 'test-token-123456789012345678901234'),
    (error) => error instanceof CoreApiError && error.status === 404 &&
      describeApiError(error).includes('lampe n’est plus connue'));

  global.fetch = async () => respond({ error: { code: 'command_api_disabled', message: 'disabled' } }, 503);
  await assert.rejects(submitPower('192.168.1.20', 11, 'on', 'test-token-123456789012345678901234'),
    (error) => error instanceof CoreApiError && error.status === 503 &&
      describeApiError(error).includes('désactivé'));

  global.fetch = async () => respond({ error: { code: 'core_busy', message: 'busy' } }, 503);
  await assert.rejects(submitPower('192.168.1.20', 11, 'on', 'test-token-123456789012345678901234'),
    (error) => error instanceof CoreApiError && error.status === 503 &&
      describeApiError(error).includes('pas disponible'));
});

test('polling progressif suit sent puis accepted jusqu’à executed', async () => {
  let clock = 0;
  const states = ['sent', 'accepted', 'executed'];
  const observed = [];
  const result = await pollCommand(404, async () => ({ command: {
    id: 404, state: states.shift(), retries: 0,
  } }), {
    now: () => clock,
    sleep: async (milliseconds) => { clock += milliseconds; },
    delaysMs: [10, 20],
    onStatus: (command) => observed.push(command.state),
  });
  assert.equal(result.kind, 'terminal');
  assert.equal(result.command.state, 'executed');
  assert.deepEqual(observed, ['sent', 'accepted', 'executed']);
});

test('polling traite failed et expired comme des états terminaux distincts', async () => {
  for (const state of ['failed', 'expired']) {
    const result = await pollCommand(505, async () => ({ command: {
      id: 505, state, retries: 2, error: state === 'expired' ? 'execution_unknown' : 'invalid_route',
    } }));
    assert.equal(result.kind, 'terminal');
    assert.equal(result.command.state, state);
  }
});

test('404 command_not_found devient un résultat incertain sans relancer le POST', async () => {
  const result = await pollCommand(606, async () => {
    throw new CoreApiError('http', 'not found', 404, 'command_not_found');
  });
  assert.equal(result.kind, 'not_found');
});

test('les erreurs réseau temporaires de suivi sont réessayées par GET', async () => {
  let clock = 0;
  let reads = 0;
  const result = await pollCommand(707, async () => {
    reads++;
    if (reads === 1) throw new CoreApiError('http', 'busy', 503, 'core_busy');
    return { command: { id: 707, state: 'executed', retries: 1 } };
  }, { now: () => clock, sleep: async (milliseconds) => { clock += milliseconds; } });
  assert.equal(result.kind, 'terminal');
  assert.equal(reads, 2);
});

test('le timeout mobile reste distinct d’un échec de commande', async () => {
  let clock = 0;
  const result = await pollCommand(808, async () => ({ command: {
    id: 808, state: 'accepted', retries: 0,
  } }), {
    timeoutMs: 25,
    delaysMs: [10],
    now: () => clock,
    sleep: async (milliseconds) => { clock += milliseconds; },
  });
  assert.equal(result.kind, 'timeout');
});

test('un verrou bloque les doubles commandes par lampe mais laisse une autre lampe agir', () => {
  const locked = new Set();
  assert.equal(acquireLampCommand(locked, 11), true);
  assert.equal(acquireLampCommand(locked, 11), false);
  assert.equal(acquireLampCommand(locked, 12), true);
  releaseLampCommand(locked, 11);
  assert.equal(acquireLampCommand(locked, 11), true);
});

test('après executed, une relecture détail fournit le last_confirmed_state actuel', async () => {
  const requests = [];
  global.fetch = async (url, options) => {
    requests.push([url, options.method]);
    if (url.endsWith('/commands/909')) return respond({ command: {
      id: 909, state: 'executed', retries: 0,
    } });
    if (url.endsWith('/devices/11')) return respond({ device: {
      ...base, id: 11, last_confirmed_state: { power: 'on', status: 'confirmed' },
    } });
    throw new Error('unexpected route');
  };
  const status = await getCommand('192.168.1.20', 909, 'test-token-123456789012345678901234');
  assert.equal(status.command.state, 'executed');
  const detail = await getDevice('192.168.1.20', 11);
  assert.deepEqual(detail.device.last_confirmed_state, { power: 'on', status: 'confirmed' });
  assert.deepEqual(requests.map(([, method]) => method), ['GET', 'GET']);
});
