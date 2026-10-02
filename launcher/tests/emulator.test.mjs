import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { Emulator, Logcat, parseLogcat, redact, formatEntry } from '../core/emulator.mjs';
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

function fakeEmulator(answer) {
  const emulator = new Emulator({ settings: { sdk: 'C:/nowhere', avd: 'test-avd' }, findPort: async () => 5580 }, 'C:/nowhere', os.tmpdir());
  clearInterval(emulator.logcat.idle);
  emulator.diskInfo = async () => ({ exists: false });
  emulator.asRoot = async (port, script) => { emulator.scripts.push(script); return { code: 0, stdout: answer, stderr: '', timedOut: false }; };
  emulator.scripts = [];
  return emulator;
}

test('storage splits each app into APK, data, cache and game files, and finds leftovers of uninstalled apps', async () => {
  const emulator = fakeEmulator([
    '@@df', '/dev/block/dm-55  65871716 59314040   6557676  91% /data',
    '@@all', 'com.example.game', 'com.oculus.horizon', 'com.android.chrome',
    '@@apps', 'com.example.game 1000 200 100 50 20000', 'com.oculus.horizon 10 1 0 1 0',
    '@@folders', '20000\t/data/media/0/Android/obb/com.example.game', '30000\t/data/media/0/Android/obb/com.example.gone',
    '24\t/data/media/0/Android/data/com.android.chrome', '8\t/data/media/0/Android/data/com.example.gone',
    '@@temp', '500\t/data/local/tmp/perf data', '@@shared', '8\t/data/media/0/Download', '@@end', ''].join('\r\n'));
  const { guest } = await emulator.storage();
  assert.equal(guest.total, 65871716 * 1024);
  assert.equal(guest.free, 6557676 * 1024);
  assert.deepEqual(guest.apps[0], { package: 'com.example.game', apk: 1000 * 1024, data: 200 * 1024, external: 100 * 1024, cache: 50 * 1024, obb: 20000 * 1024, total: 21300 * 1024, component: false });
  assert.equal(guest.apps[1].component, true);
  assert.deepEqual(guest.leftovers.map(f => [f.path, f.kind]), [
    ['/data/media/0/Android/obb/com.example.gone', 'obb'], ['/data/local/tmp/perf data', 'temp'], ['/data/media/0/Android/data/com.example.gone', 'data']]);
});

test('deleting only takes leftovers of uninstalled apps and /data/local/tmp files, quoted for the shell', async () => {
  const emulator = fakeEmulator('@@done');
  emulator.adb = async () => 'package:com.example.game\npackage:com.android.chrome\n';
  for (const bad of ['/data/media/0/Android/obb/com.example.game', '/data/media/0/Android/data/com.android.chrome', '/data/local/tmp/..', '/data/local/tmp/a/b',
    '/data/local/tmp/../app', '/data/app/x', '/data/media/0/Android/obb/a;b', 7]) {
    await assert.rejects(emulator.deleteFiles(5580, [bad]), /does not delete/);
  }
  await assert.rejects(emulator.deleteFiles(5580, []), /Select files/);
  assert.equal(emulator.scripts.length, 0);
  await emulator.deleteFiles(5580, ['/data/media/0/Android/obb/com.example.gone', "/data/local/tmp/it's here"]);
  assert.equal(emulator.scripts[0], "rm -rf '/data/media/0/Android/obb/com.example.gone' '/data/local/tmp/it'\\''s here'; echo @@done");
});

test('Reset Android sets the new disk size and marks the next boot to wipe /data', async t => {
  const home = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-avd-'));
  const old = process.env.ANDROID_AVD_HOME;
  process.env.ANDROID_AVD_HOME = home;
  t.after(async () => { if (old === undefined) delete process.env.ANDROID_AVD_HOME; else process.env.ANDROID_AVD_HOME = old; await fs.rm(home, { recursive: true, force: true }); });
  const directory = path.join(home, 'test-avd.avd');
  await fs.mkdir(directory);
  await fs.writeFile(path.join(home, 'test-avd.ini'), `avd.ini.encoding=UTF-8\r\npath=${directory}\r\n`);
  await fs.writeFile(path.join(directory, 'config.ini'), 'hw.ramSize=8192\r\ndisk.dataPartition.size=64G\r\nhw.useext4=yes\r\n');
  const emulator = fakeEmulator('');
  await assert.rejects(emulator.scheduleReset(50), /disk size/);
  await emulator.scheduleReset(128);
  assert.equal(await fs.readFile(path.join(directory, 'config.ini'), 'utf8'), 'hw.ramSize=8192\r\ndisk.dataPartition.size=128G\r\nhw.useext4=yes\r\n');
  assert.equal(await fs.readFile(path.join(directory, 'refract-first-boot-pending'), 'utf8'), 'wipe-data');
  delete emulator.diskInfo;
  const disk = await emulator.diskInfo();
  assert.equal(disk.dataSize, 128 * 1024 ** 3);
  assert.equal(disk.resetPending, true);
});

test('Make the disk bigger resizes the disk file, records the size and marks the grow boot; never shrinks', async t => {
  const qemuImg = [path.join(os.homedir(), 'Android/Sdk/emulator/qemu-img.exe'), path.join(process.env.LOCALAPPDATA || '', 'Android/Sdk/emulator/qemu-img.exe')];
  const tool = (await Promise.all(qemuImg.map(f => fs.access(f).then(() => f, () => '')))).find(Boolean);
  if (!tool) { t.skip('no Android emulator qemu-img on this PC'); return; }
  const home = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-avd-'));
  const old = process.env.ANDROID_AVD_HOME;
  process.env.ANDROID_AVD_HOME = home;
  t.after(async () => { if (old === undefined) delete process.env.ANDROID_AVD_HOME; else process.env.ANDROID_AVD_HOME = old; await fs.rm(home, { recursive: true, force: true }); });
  const directory = path.join(home, 'test-avd.avd');
  await fs.mkdir(directory);
  await fs.writeFile(path.join(home, 'test-avd.ini'), `path=${directory}\r\n`);
  await fs.writeFile(path.join(directory, 'config.ini'), 'disk.dataPartition.size=16G\r\n');
  const { run } = await import('../core/runtime.mjs');
  await run(tool, ['create', '-f', 'qcow2', path.join(directory, 'userdata-qemu.img.qcow2'), '16G']);
  const emulator = fakeEmulator('');
  emulator.qemuImg = () => tool;
  await assert.rejects(emulator.enlargeDisk(16), /already 16 GB/);
  await emulator.enlargeDisk(32);
  assert.equal(await emulator.diskSize(), 32 * 1024 ** 3);
  assert.equal(await fs.readFile(path.join(directory, 'config.ini'), 'utf8'), 'disk.dataPartition.size=32G\r\n');
  await emulator.markGrowPending();
  delete emulator.diskInfo;
  const disk = await emulator.diskInfo();
  assert.equal(disk.dataSize, 32 * 1024 ** 3);
  assert.equal(disk.growPending, true);
});
