import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { androidPackages, createAvd, missingAndroid, patchMulticore } from '../core/android_sdk.mjs';

test('an empty SDK folder needs every pinned Android package', async t => {
  const sdk = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-sdk-'));
  t.after(() => fs.rm(sdk, { recursive: true, force: true }));
  assert.deepEqual((await missingAndroid(sdk)).map(p => p.id), androidPackages.map(p => p.id));
  // A different emulator build or image revision is not the tested one.
  await fs.mkdir(path.join(sdk, 'emulator'), { recursive: true });
  await fs.writeFile(path.join(sdk, 'emulator/source.properties'), 'Pkg.Revision=37.3.2\nPkg.BuildId=16433917\n');
  assert.ok((await missingAndroid(sdk)).some(p => p.id === 'emulator'));
  await fs.writeFile(path.join(sdk, 'emulator/source.properties'), 'Pkg.Revision=37.1.11\nPkg.BuildId=15917651\n');
  assert.ok(!(await missingAndroid(sdk)).some(p => p.id === 'emulator'));
});

test('the virtual device is created with the tested hardware settings', async t => {
  const home = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-avd-'));
  const previous = process.env.ANDROID_AVD_HOME;
  process.env.ANDROID_AVD_HOME = home;
  t.after(async () => { process.env.ANDROID_AVD_HOME = previous ?? ''; if (previous === undefined) delete process.env.ANDROID_AVD_HOME; await fs.rm(home, { recursive: true, force: true }); });
  await createAvd('refract-test');
  const ini = await fs.readFile(path.join(home, 'refract-test.ini'), 'utf8');
  assert.match(ini, new RegExp(`^path=${path.join(home, 'refract-test.avd').replace(/\\/g, '\\\\')}\r$`, 'm'));
  const config = await fs.readFile(path.join(home, 'refract-test.avd/config.ini'), 'utf8');
  for (const line of ['image.sysdir.1=system-images\\android-36\\google_apis\\x86_64\\', 'hw.gltransport=asg', 'hw.gpu.mode=host', 'hw.cpu.ncore=6'])
    assert.ok(config.split('\r\n').includes(line), line);
});

test('the multi-core patch only touches the tested emulator build', async t => {
  const sdk = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-sdk-'));
  t.after(() => fs.rm(sdk, { recursive: true, force: true }));
  const qemu = path.join(sdk, 'emulator/qemu/windows-x86_64');
  await fs.mkdir(qemu, { recursive: true });
  await fs.writeFile(path.join(qemu, 'qemu-system-x86_64.exe'), Buffer.from('not the pinned emulator'));
  await assert.rejects(patchMulticore(sdk), /not build 15917651/);
  await assert.rejects(fs.access(path.join(qemu, 'qemu-system-x86_64-multicore.exe')));
});
