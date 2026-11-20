import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import ts from 'typescript';

const source = await readFile(new URL('../src/meter.ts', import.meta.url), 'utf8');
const { outputText } = ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 },
});
const { advanceMeterBar, litMeterSegments, MeterStore } = await import(
  `data:text/javascript;base64,${Buffer.from(outputText).toString('base64')}`
);

test('meter bar falls 20 dB in 1.7 seconds independently of sample spacing', () => {
  assert.ok(Math.abs(advanceMeterBar(0, -Infinity, 1700) + 20) < 1e-10);
  let level = 0;
  for (const elapsed of [5, 23, 100, 72, 500, 1000]) {
    level = advanceMeterBar(level, -Infinity, elapsed);
  }
  assert.ok(Math.abs(level + 20) < 1e-10);
});

test('meter bar attacks immediately and never falls below the live peak', () => {
  assert.equal(advanceMeterBar(-40, -3, 10), -3);
  assert.equal(advanceMeterBar(-10, -12, 1000), -12);
});

test('meter bar reaches silence at the bottom of the display', () => {
  assert.equal(advanceMeterBar(-59, -Infinity, 1700), -Infinity);
  assert.equal(advanceMeterBar(-Infinity, -Infinity, 100), -Infinity);
});

test('meter segments preserve the existing threshold behavior', () => {
  assert.equal(litMeterSegments(-Infinity), 0);
  assert.equal(litMeterSegments(-60), 1);
  assert.equal(litMeterSegments(-12), 49);
  assert.equal(litMeterSegments(-3), 57);
  assert.equal(litMeterSegments(-2.9), 58);
  assert.equal(litMeterSegments(0), 60);
});

test('meter updates notify only the corresponding input bank and retain the other bank', () => {
  const store = new MeterStore();
  const notified = [];
  store.subscribe('input', 0, () => notified.push('first'));
  store.subscribe('input', 48, () => notified.push('second'));
  const values = Array.from({ length: 52 }, (_, index) => index);
  store.publish(0, 3, values, 1);
  assert.deepEqual(notified, ['first']);
  assert.deepEqual(store.get('input', 0), { value: 0, sequence: 1 });
  assert.equal(store.get('input', 48), null);
  store.publish(1, 3, values, 2);
  assert.deepEqual(notified, ['first', 'second']);
  assert.deepEqual(store.get('input', 0), { value: 0, sequence: 1 });
  assert.deepEqual(store.get('input', 48), { value: 0, sequence: 2 });
});

test('output samples belong only to their selected bus and reset clears snapshots', () => {
  const store = new MeterStore();
  let notifications = 0;
  const unsubscribe = store.subscribe('output', 0, () => { notifications += 1; });
  const values = Array.from({ length: 52 }, (_, index) => index);
  store.publish(0, 2, values, 7);
  assert.deepEqual(store.get('output', 0, 2), { value: 48, sequence: 7 });
  assert.equal(store.get('output', 0, 3), null);
  store.publish(0, 3, values, 8);
  assert.equal(store.get('output', 0, 2), null);
  assert.deepEqual(store.get('output', 0, 3), { value: 48, sequence: 8 });
  store.reset();
  assert.equal(store.get('input', 0), null);
  assert.equal(store.get('output', 0, 3), null);
  assert.equal(notifications, 3);
  unsubscribe();
  store.publish(0, 3, values, 9);
  assert.equal(notifications, 3);
});
