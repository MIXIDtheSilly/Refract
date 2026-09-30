import test from 'node:test';
import assert from 'node:assert/strict';
import { Logcat, parseLogcat, redact, formatEntry } from '../core/emulator.mjs';
import { emulatorOptions, powershellArgs } from '../core/runtime.mjs';

test('logcat threadtime lines split into time, pid, tid, level, tag and message', () => {
  assert.deepEqual(parseLogcat('09-30 16:21:31.722  1234  1250 I Unity   : Loaded scene: Main'),
    { time: '09-30 16:21:31.722', pid: 1234, tid: 1250, level: 'I', tag: 'Unity', message: 'Loaded scene: Main' });
  assert.deepEqual(parseLogcat('09-30 16:21:31.722   512   530 E AndroidRuntime: FATAL EXCEPTION: main'),
    { time: '09-30 16:21:31.722', pid: 512, tid: 530, level: 'E', tag: 'AndroidRuntime', message: 'FATAL EXCEPTION: main' });
  assert.equal(parseLogcat('not a logcat line').message, 'not a logcat line');
  assert.equal(parseLogcat('not a logcat line').level, '-');
});

test('the logcat buffer hands out only new lines, and a reset when the page falls behind or it was cleared', async () => {
  const logcat = new Logcat({ settings: { sdk: 'C:/nowhere' }, findPort: async () => null }, 10);
  clearInterval(logcat.idle);
  for (let i = 0; i < 5; i++) logcat.push(parseLogcat(`09-30 16:21:31.72${i}  1  2 D Tag: line ${i}`));
  let result = logcat.read(0, 0, false);
  assert.equal(result.reset, true);
  assert.equal(result.entries.length, 5);
  const last = result.entries.at(-1)[0];
  for (let i = 5; i < 7; i++) logcat.push(parseLogcat(`09-30 16:21:32.72${i}  1  2 D Tag: line ${i}`));
  result = logcat.read(last, result.generation, false);
  assert.equal(result.reset, false);
  assert.deepEqual(result.entries.map(e => e[6]), ['line 5', 'line 6']);
  // More lines than the buffer holds: a page still at `last` has missed some and starts over.
  for (let i = 0; i < 20; i++) logcat.push(parseLogcat(`09-30 16:21:33.000  1  2 D Tag: more ${i}`));
  result = logcat.read(last, result.generation, false);
  assert.equal(result.reset, true);
  assert.ok(result.entries.length <= 11);
  const generation = result.generation;
  await logcat.clear();
  result = logcat.read(result.entries.at(-1)[0], generation, false);
  assert.equal(result.reset, true);
  assert.equal(result.entries.length, 0);
});

test('logcat and log text never carries Meta tokens', () => {
  const logcat = new Logcat({ settings: { sdk: 'C:/nowhere' } });
  clearInterval(logcat.idle);
  logcat.push(parseLogcat(`09-30 16:21:31.722  1  2 I Platform: token FRL${'a'.repeat(40)} access_token=abc123&x=1`));
  const line = formatEntry(logcat.entries[0]);
  assert.doesNotMatch(line, /FRLa|abc123/);
  assert.match(line, /\[redacted\]/);
  assert.equal(redact('OC' + 'x'.repeat(40)), '[redacted]');
});

test('emulator settings become start script parameters; switches only when on', () => {
  assert.deepEqual(emulatorOptions({}), { Cores: 6, Audio: 'dsound' });
  assert.deepEqual(emulatorOptions({ cores: 2, audio: 'sdl', showWindow: true, hostMic: false }), { Cores: 2, Audio: 'sdl', ShowWindow: true, NoHostMic: true });
  const script = Buffer.from(powershellArgs('x.ps1', emulatorOptions({ showWindow: false, hostMic: true })).at(-1), 'base64').toString('utf16le');
  assert.doesNotMatch(script, /ShowWindow|NoHostMic/);
});
