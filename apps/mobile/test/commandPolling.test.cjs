const assert = require('node:assert/strict');
const fs = require('node:fs');
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

const { acquireLampCommand, canInterruptWithPriorityOff,
  isCurrentLampCommand, releaseLampCommand } = require(
  '../src/features/lighting/commandPolling.ts');

test('OFF prioritaire permis uniquement pendant le suivi d’une autre sortie', () => {
  assert.equal(canInterruptWithPriorityOff({ phase: 'tracking', requested: 'on',
    commandId: 10, commandState: 'accepted', retries: 0 }), true);
  assert.equal(canInterruptWithPriorityOff({ phase: 'tracking', requested: 'off',
    brightnessPercent: 0, commandId: 11, commandState: 'sent', retries: 0 }), true);
  assert.equal(canInterruptWithPriorityOff({ phase: 'tracking', requested: 'on',
    brightnessPercent: 100, commandId: 13, commandState: 'accepted', retries: 0 }), true);
  assert.equal(canInterruptWithPriorityOff({ phase: 'tracking', requested: 'off',
    commandId: 12, commandState: 'sent', retries: 0 }), false);
  assert.equal(canInterruptWithPriorityOff({ phase: 'submitting', requested: 'on' }), false);
  assert.equal(canInterruptWithPriorityOff({ phase: 'uncertain', requested: 'on',
    message: 'timeout' }), false);
});

test('le verrou par lampe empêche un double POST OFF et suit le dernier propriétaire', () => {
  const lampId = 42;
  const locks = new Set();
  const owners = new Map();
  const brightness = new AbortController();
  const off = new AbortController();

  assert.equal(acquireLampCommand(locks, lampId), true);
  owners.set(lampId, brightness);
  assert.equal(acquireLampCommand(locks, lampId), false);
  owners.set(lampId, off);
  brightness.abort();
  assert.equal(isCurrentLampCommand(owners, lampId, brightness), false);
  assert.equal(isCurrentLampCommand(owners, lampId, off), true);
  releaseLampCommand(locks, lampId);
  assert.equal(acquireLampCommand(locks, lampId), true);
});

test('la vue expose un contrôle OFF prioritaire sans débloquer ON ni le curseur', () => {
  const lampCard = fs.readFileSync(path.join(__dirname,
    '../src/features/lighting/LampCard.tsx'), 'utf8');
  assert.match(lampCard, /canInterruptWithPriorityOff\(feedback\)/);
  assert.match(lampCard, /disabled=\{!\(canControl \|\| canPriorityOff\)\}/);
  assert.match(lampCard, /disabled=\{!canControlLampBrightness\}/);
  const app = fs.readFileSync(path.join(__dirname, '../App.tsx'), 'utf8');
  assert.match(app, /return supersededByNewerOutput \? null : updatedLamp/);
});
