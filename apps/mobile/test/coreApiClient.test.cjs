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
  getDevice, getCommand, lampsOnly, submitPower } = require('../src/api/coreApiClient.ts');
const { acquireLampCommand, pollCommand, releaseLampCommand } =
  require('../src/features/lighting/commandPolling.ts');
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
      { ...base, id: 11, last_confirmed_state: { power: 'on', status: 'confirmed' } },
      { ...base, id: 12, name: 'lamp002', online: false, status: 'offline',
        last_confirmed_state: { power: 'off', status: 'stale' } },
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
