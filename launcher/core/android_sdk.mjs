// Installs the Android pieces Refract is tested with straight from Google's SDK repository (no Android Studio,
// Java or sdkmanager), then creates the virtual device. Versions are pinned on purpose: the Digitalis translator
// is built against this system image's Android build, and the multi-core patch matches this emulator build.
import { createHash } from 'node:crypto';
import { createReadStream, createWriteStream } from 'node:fs';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { Readable, Transform } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { run } from './runtime.mjs';

const REPOSITORY = 'https://dl.google.com/android/repository/';
const exists = file => fs.access(file).then(() => true, () => false);
const property = async (file, key) => (await fs.readFile(file, 'utf8').catch(() => '')).match(new RegExp(`^${key.replace('.', '\\.')}=(.*)$`, 'm'))?.[1]?.trim();

// Checksums and sizes are the ones Google's repository lists for each archive (repository2-3.xml, sys-img2-3.xml).
export const androidPackages = [
  { id: 'platform-tools', label: 'Android platform tools', file: 'platform-tools_r37.0.1-win.zip', sha1: 'e03e78b1d80b396f1c3358e31251cb31740e1110', size: 8044989,
    dir: 'platform-tools', ready: sdk => exists(path.join(sdk, 'platform-tools/adb.exe')) },
  { id: 'build-tools', label: 'Android build tools 36', file: 'build-tools_r36_windows.zip', sha1: 'f16ccffd34de8790dede813a6c7d8e2c11a27b50', size: 58699878,
    dir: 'build-tools/36.0.0', ready: async sdk => {
      const versions = await fs.readdir(path.join(sdk, 'build-tools')).catch(() => []);
      return (await Promise.all(versions.map(v => exists(path.join(sdk, 'build-tools', v, 'aapt2.exe'))))).some(Boolean);
    } },
  { id: 'emulator', label: 'Android Emulator 37.1.11', file: 'emulator-windows_x64-15917651.zip', sha1: '54fa750822ff462d57e04fc8e98e60f08df2bb61', size: 441926448,
    dir: 'emulator', ready: async sdk => await property(path.join(sdk, 'emulator/source.properties'), 'Pkg.BuildId') === '15917651' },
  { id: 'image', label: 'Android 16 system image', file: 'sys-img/google_apis/x86_64-36_r07.zip', sha1: 'c6bf44bdcd885bb902b4ba752d111a073ad7a817', size: 1895447397,
    dir: 'system-images/android-36/google_apis/x86_64', ready: async sdk => await property(path.join(sdk, 'system-images/android-36/google_apis/x86_64/source.properties'), 'Pkg.Revision') === '7' },
];

// On Intel + Windows Hypervisor Platform the stock emulator decides "not all modern x86 virtualization features"
// are present and runs Android on one vCPU. A copy with that check's jump NOPed (tools/patch_emulator_cores.py)
// uses the AVD's cores. Only for emulator 37.1.11 build 15917651; the original file is left alone.
const QEMU = 'emulator/qemu/windows-x86_64/qemu-system-x86_64.exe';
export const MULTICORE = 'emulator/qemu/windows-x86_64/qemu-system-x86_64-multicore.exe';
const QEMU_SHA256 = '2077f28c713f5872734ef438c83211c00afbe9022a14cf8874891b5d0a50df32';
const MULTICORE_SHA256 = '4b348d398c03e6947a50e22078ac51149eaa76f7270d9fb6698deade89d7426c';
const CORE_CHECK = Buffer.from('e85f3f3a0084c07466', 'hex');  // call hasModernX86VirtualizationFeatures; test al, al; je

export async function multicoreReady(sdk) { return exists(path.join(sdk, MULTICORE)); }
export async function patchMulticore(sdk) {
  const data = await fs.readFile(path.join(sdk, QEMU));
  if (createHash('sha256').update(data).digest('hex') !== QEMU_SHA256) throw new Error('The installed emulator is not build 15917651, so the multi-core patch does not apply.');
  const at = data.indexOf(CORE_CHECK);
  if (at < 0 || data.indexOf(CORE_CHECK, at + 1) >= 0) throw new Error('The multi-core patch location was not found.');
  data[at + 7] = 0x90; data[at + 8] = 0x90;
  if (createHash('sha256').update(data).digest('hex') !== MULTICORE_SHA256) throw new Error('The multi-core patch produced an unexpected file.');
  await fs.writeFile(path.join(sdk, MULTICORE), data);
}

export function avdHome() {
  if (process.env.ANDROID_AVD_HOME) return process.env.ANDROID_AVD_HOME;
  if (process.env.ANDROID_USER_HOME) return path.join(process.env.ANDROID_USER_HOME, 'avd');
  return path.join(os.homedir(), '.android/avd');
}

// The virtual device Refract runs games in, like the one it was developed on: 6 cores, 8 GB, host GPU,
// shared-memory graphics transport (hw.gltransport=asg), room for large games.
export async function createAvd(name) {
  const home = avdHome(), dir = path.join(home, `${name}.avd`);
  await fs.mkdir(dir, { recursive: true });
  const config = {
    'avd.ini.encoding': 'UTF-8', 'AvdId': name, 'avd.ini.displayname': name, 'PlayStore.enabled': 'no', 'abi.type': 'x86_64', 'hw.cpu.arch': 'x86_64',
    'image.sysdir.1': 'system-images\\android-36\\google_apis\\x86_64\\', 'tag.id': 'google_apis', 'tag.display': 'Google APIs', 'target': 'android-36',
    'hw.cpu.ncore': '6', 'hw.ramSize': '8192', 'vm.heapSize': '228M', 'disk.dataPartition.size': '32G', 'userdata.useQcow2': 'no', 'hw.useext4': 'yes',
    'hw.gpu.enabled': 'yes', 'hw.gpu.mode': 'host', 'hw.gltransport': 'asg', 'hw.gltransport.asg.dataRingSize': '32768',
    'hw.gltransport.asg.writeBufferSize': '1048576', 'hw.gltransport.asg.writeStepSize': '4096', 'hw.gltransport.drawFlushInterval': '800',
    'hw.lcd.width': '1080', 'hw.lcd.height': '1920', 'hw.lcd.density': '420', 'hw.keyboard': 'yes', 'hw.mainKeys': 'no',
    'hw.audioInput': 'yes', 'hw.audioOutput': 'yes', 'hw.sdCard': 'no', 'hw.camera.back': 'none', 'hw.camera.front': 'none',
    'fastboot.forceColdBoot': 'yes', 'showDeviceFrame': 'no',
  };
  await fs.writeFile(path.join(dir, 'config.ini'), Object.entries(config).map(([k, v]) => `${k}=${v}\r\n`).join(''));
  // Its first boot must run on the stock single-core emulator (tools/windows_android_emulator.ps1 removes this).
  await fs.writeFile(path.join(dir, 'refract-first-boot-pending'), '');
  await fs.writeFile(path.join(home, `${name}.ini`), `avd.ini.encoding=UTF-8\r\npath=${dir}\r\ntarget=android-36\r\n`);
}

// Downloads one archive into <sdk>\.refract-downloads, resuming a partial file, and checks its SHA-1.
async function download(sdk, item, progress) {
  const folder = path.join(sdk, '.refract-downloads');
  await fs.mkdir(folder, { recursive: true });
  const file = path.join(folder, path.basename(item.file));
  if (await exists(file)) return file;
  const part = `${file}.part`;
  for (let attempt = 1; ; attempt++) {
    try {
      const hash = createHash('sha1');
      let have = (await fs.stat(part).catch(() => null))?.size || 0;
      if (have) await pipeline(createReadStream(part), new Transform({ transform(chunk, _, done) { hash.update(chunk); done(); } }));
      const response = await fetch(REPOSITORY + item.file, { headers: have ? { Range: `bytes=${have}-` } : {} });
      if (have && response.status !== 206) { await fs.rm(part, { force: true }); have = 0; hash.destroy(); throw new Error('The server did not resume the download.'); }
      if (!response.ok) throw new Error(`Download failed (HTTP ${response.status}).`);
      let done = have, last = 0;
      await pipeline(Readable.fromWeb(response.body), new Transform({
        transform(chunk, _, next) {
          hash.update(chunk); done += chunk.length;
          if (Date.now() - last > 500) { last = Date.now(); progress(`Downloading ${item.label} (${Math.floor(done / item.size * 100)}% of ${(item.size / 1e9).toFixed(item.size > 1e9 ? 1 : 2)} GB)`); }
          next(null, chunk);
        },
      }), createWriteStream(part, { flags: have ? 'a' : 'w' }));
      if (hash.digest('hex') !== item.sha1) { await fs.rm(part, { force: true }); throw new Error(`${item.label} was damaged in download.`); }
      await fs.rename(part, file);
      return file;
    } catch (error) {
      if (attempt >= 4) throw new Error(`Could not download ${item.label}: ${error.message} Check your internet connection and try again.`);
      progress(`Download interrupted, retrying ${item.label}`);
      await new Promise(resolve => setTimeout(resolve, 3000 * attempt));
    }
  }
}

// Unpacks an archive (Windows' tar reads zip) and moves its single top folder to <sdk>\<item.dir>.
async function extract(sdk, item, zip, progress) {
  progress(`Unpacking ${item.label}`);
  const staging = path.join(sdk, '.refract-downloads', `${item.id}.unpack`);
  await fs.rm(staging, { recursive: true, force: true });
  await fs.mkdir(staging, { recursive: true });
  await run(path.join(process.env.SystemRoot || 'C:\\Windows', 'System32/tar.exe'), ['-xf', zip, '-C', staging], { timeout: 30 * 60 * 1000 });
  const entries = await fs.readdir(staging);
  if (entries.length !== 1) throw new Error(`${item.label} has an unexpected layout.`);
  const target = path.join(sdk, item.dir);
  await fs.mkdir(path.dirname(target), { recursive: true });
  // An older package in the way is replaced, as sdkmanager does on an update.
  await fs.rm(target, { recursive: true, force: true });
  await fs.rename(path.join(staging, entries[0]), target);
  await fs.rm(staging, { recursive: true, force: true });
  await fs.rm(zip, { force: true });
}

export async function missingAndroid(sdk) {
  const missing = [];
  for (const item of androidPackages) if (!await item.ready(sdk)) missing.push(item);
  return missing;
}

// Everything the "Set up Android" button does. progress(text) reports each step.
export async function installAndroid(sdk, avd, progress = () => {}) {
  await fs.mkdir(sdk, { recursive: true });
  for (const item of await missingAndroid(sdk)) await extract(sdk, item, await download(sdk, item, progress), progress);
  if (!await multicoreReady(sdk)) { progress('Enabling multi-core Android'); await patchMulticore(sdk).catch(() => {}); }
  if (!await exists(path.join(avdHome(), `${avd}.ini`))) { progress('Creating the virtual device'); await createAvd(avd); }
  await fs.rm(path.join(sdk, '.refract-downloads'), { recursive: true, force: true });
}
