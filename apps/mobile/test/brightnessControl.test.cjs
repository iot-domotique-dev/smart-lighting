const assert = require('node:assert/strict');
const fs = require('node:fs');
const { test } = require('node:test');
const ts = require('typescript');

require.extensions['.ts'] = (module, filename) => {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  module._compile(output, filename);
};

const { brightnessDisplay, brightnessSliderKey, canControlBrightness, normalizeBrightnessPercent,
  shouldSubmitBrightnessOnRelease, supportsBrightness } =
  require('../src/features/lighting/brightnessControl.ts');
const { acquireLampCommand, releaseLampCommand } =
  require('../src/features/lighting/commandPolling.ts');

const lamp = { role: 'lamp', capabilities: ['power', 'brightness'] };

test('brightness est disponible seulement avec la capacité annoncée', () => {
  assert.equal(supportsBrightness(lamp), true);
  assert.equal(supportsBrightness({ ...lamp, capabilities: ['power'] }), false);
  assert.equal(supportsBrightness({ ...lamp, role: 'sensor' }), false);
});

test('luminosité désactivée si la lampe, le MAIN, les données ou la commande sont indisponibles', () => {
  assert.equal(canControlBrightness({ ...lamp, online: true }, true, true, false), true);
  assert.equal(canControlBrightness({ ...lamp, online: false }, true, true, false), false);
  assert.equal(canControlBrightness({ ...lamp, online: true }, false, true, false), false);
  assert.equal(canControlBrightness({ ...lamp, online: true }, true, false, false), false);
  assert.equal(canControlBrightness({ ...lamp, online: true }, true, true, true), false);
});

test('les valeurs confirmed, unknown, stale et historique absent restent distinctes', () => {
  assert.deepEqual(brightnessDisplay({ power: 'on', status: 'confirmed',
    brightness_percent: 37, brightness_status: 'confirmed' }),
  { percent: 37, status: 'confirmed' });
  assert.deepEqual(brightnessDisplay({ power: null, status: 'unknown',
    brightness_percent: null, brightness_status: 'unknown' }),
  { percent: null, status: 'unknown' });
  assert.deepEqual(brightnessDisplay({ power: 'off', status: 'stale',
    brightness_percent: 25, brightness_status: 'stale' }),
  { percent: 25, status: 'stale' });
  assert.deepEqual(brightnessDisplay({ power: 'on', status: 'confirmed' }),
  { percent: null, status: 'unknown' });
});

test('la clé native du curseur change quand l’état de luminosité confirmé change', () => {
  const offAtZero = brightnessSliderKey(11, { percent: 0, status: 'confirmed' });
  const onAtPreviousLevel = brightnessSliderKey(11,
    { percent: 62, status: 'confirmed' });
  assert.notEqual(offAtZero, onAtPreviousLevel);
  assert.equal(onAtPreviousLevel,
    brightnessSliderKey(11, { percent: 62, status: 'confirmed' }));
  assert.notEqual(onAtPreviousLevel,
    brightnessSliderKey(11, { percent: 62, status: 'stale' }));
});

test('la conversion de curseur ne produit que des entiers de 0 à 100', () => {
  assert.equal(normalizeBrightnessPercent(0), 0);
  assert.equal(normalizeBrightnessPercent(1.4), 1);
  assert.equal(normalizeBrightnessPercent(50.6), 51);
  assert.equal(normalizeBrightnessPercent(100), 100);
  assert.equal(normalizeBrightnessPercent(-5), 0);
  assert.equal(normalizeBrightnessPercent(105), 100);
});

test('la sélection ne déclenche rien pendant le glissement et évite le POST au relâchement sans changement', () => {
  const requests = [];
  let selected = 0;
  for (const value of [10, 27, 50]) selected = normalizeBrightnessPercent(value);
  assert.deepEqual(requests, []);
  if (shouldSubmitBrightnessOnRelease(0, selected, 20, 'confirmed')) requests.push(selected);
  assert.deepEqual(requests, [50]);
  assert.equal(shouldSubmitBrightnessOnRelease(50, 50, 50, 'confirmed'), false);
  assert.equal(shouldSubmitBrightnessOnRelease(50, 50, 50, 'stale'), false);
  assert.equal(shouldSubmitBrightnessOnRelease(0, 0, null, 'unknown'), false);
});

test('0, 1, 50 et 100 restent des consignes distinctes au relâchement', () => {
  for (const percent of [0, 1, 50, 100]) {
    const start = percent === 0 ? 1 : 0;
    assert.equal(shouldSubmitBrightnessOnRelease(start, percent, null, 'unknown'), true);
  }
});

test('le verrou unique d’une lampe arbitre POWER et BRIGHTNESS, sans bloquer une autre lampe', () => {
  const locks = new Set();
  assert.equal(acquireLampCommand(locks, 11), true); // POWER ou BRIGHTNESS
  assert.equal(acquireLampCommand(locks, 11), false); // l’autre commande est bloquée
  assert.equal(acquireLampCommand(locks, 12), true);
  releaseLampCommand(locks, 11);
  assert.equal(acquireLampCommand(locks, 11), true);
});
