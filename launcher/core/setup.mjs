// Checks that everything Refract needs to run a game is on this PC, and fixes what it can.
// Each check: { id, title, ok, detail, fix?: { action, label, note? } }.
import fs from 'node:fs/promises';
import path from 'node:path';
import { run, powershellArgs, guestPackages, nativeExe, nativeSysroot } from './runtime.mjs';
import { avdHome, installAndroid, installBuildTools, installNativeImage, missingAndroid, multicoreReady } from './android_sdk.mjs';

const exists = file => fs.access(file).then(() => true, () => false);
const winget = id => ['install', '--exact', '--id', id, '--silent', '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity'];
// A tool installed while the launcher runs is on the user's PATH in the registry, not in this process.
export async function refreshPath() {
  const script = "[Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')";
  const value = (await run('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script], { timeout: 20000 }).catch(() => '')).trim();
  if (value) process.env.Path = process.env.PATH = value;
}

// Build outputs a game session uses, with the script that makes each.
function components(root, sdk, jdk) {
  return [
    { label: 'Host bridge', file: 'build-windows-nvidia/host-bridge/refract-host-bridge.exe', build: ['cmd.exe', ['/d', '/c', path.join(root, 'build_host.cmd')]] },
    { label: 'PC viewer', file: 'viewer/build/refract_viewer.exe', build: ['cmd.exe', ['/d', '/c', path.join(root, 'viewer/build.bat')], { env: { ANDROID_SDK_ROOT: sdk } }] },
    { label: 'GPU sharing layer', file: 'build-windows-gpu-layer/Release/refract_gpu_layer.dll', build: ['powershell.exe', powershellArgs(path.join(root, 'tools/windows_gpu_layer/build.ps1'), {})] },
    ...guestPackages.map(p => {
      const parameters = p.package === 'com.oculus.horizon' ? { Sdk: sdk } : { Sdk: sdk, Abi: 'arm64-v8a' };
      if (jdk) parameters.Jdk = jdk;
      return { label: p.label, file: p.apk, build: ['powershell.exe', powershellArgs(path.join(root, p.package === 'com.oculus.horizon' ? 'platform-sdk/build_apk.ps1' : 'android-runtime-apk/build_apk.ps1'), parameters)] };
    }),
    { label: 'ARM translator (Digitalis)', file: 'prebuilts/digitalis/system' },
  ];
}

async function python() {
  const check = { id: 'python', title: 'Python' };
  try {
    const version = (await run('python', ['--version'], { timeout: 15000 })).trim();
    if (/^Python 3\./.test(version)) return { ...check, ok: true, detail: version };
  } catch { /* Missing, or the Microsoft Store placeholder. */ }
  return { ...check, ok: false, detail: 'Refract reads game packages and prepares Android with Python 3.', fix: { action: 'python', label: 'Install Python' } };
}

// The Android SDK packages Refract is tested with (pinned, see android_sdk.mjs) and its virtual device.
async function android(sdk, avd) {
  const sdkCheck = { id: 'sdk', title: 'Android' };
  const avdCheck = { id: 'avd', title: 'Virtual device' };
  const missing = await missingAndroid(sdk);
  const gb = missing.reduce((sum, item) => sum + item.size, 0) / 1e9;
  const setup = { action: 'android', label: 'Set up Android',
    note: `Downloads ${gb >= 0.1 ? `about ${gb.toFixed(1)} GB` : 'a few files'} from Google. Setting up accepts the Android SDK License Agreement (developer.android.com/studio/terms).` };
  const cores = missing.some(m => m.id === 'emulator') || await multicoreReady(sdk);
  const results = [missing.length || !cores
    ? { ...sdkCheck, ok: false, detail: missing.length ? `Needs ${missing.map(m => m.label).join(', ')} in ${sdk}.` : 'The emulator would give Android only one CPU core.', fix: setup }
    : { ...sdkCheck, ok: true, detail: `Emulator 37.1.11 with multi-core Android, in ${sdk}` }];
  const device = await exists(path.join(avdHome(), `${avd}.ini`));
  results.push(device ? { ...avdCheck, ok: true, detail: `${avd} (Android 16)` }
    : { ...avdCheck, ok: false, detail: `There is no virtual device named ${avd} yet.`, fix: setup });
  return results;
}

export async function hypervisor(sdk) {
  const check = { id: 'hypervisor', title: 'Hardware acceleration' };
  const emulator = path.join(sdk, 'emulator/emulator.exe');
  if (!await exists(emulator)) return { ...check, ok: false, detail: 'Needs the Android emulator first.' };
  try {
    const output = await run(emulator, ['-accel-check'], { timeout: 30000 });
    return { ...check, ok: true, detail: output.split(/\r?\n/).find(l => /WHPX|AEHD|hypervisor/i.test(l))?.trim() || 'Available' };
  } catch (error) {
    return { ...check, ok: false, detail: `The emulator cannot use hardware acceleration: ${String(error.message).split('\n').pop()}`,
      fix: { action: 'hypervisor', label: 'Turn on Windows Hypervisor Platform', note: 'Windows asks for permission. Restart Windows afterwards.' } };
  }
}

async function built(root, sdk) {
  const check = { id: 'components', title: 'Refract components' };
  const missing = [];
  for (const item of components(root, sdk)) if (!await exists(path.join(root, item.file))) missing.push(item);
  if (!missing.length) return { ...check, ok: true, detail: 'Host bridge, GPU sharing layer, Android runtime and platform stand-in are built.' };
  const buildable = missing.every(m => m.build);
  return { ...check, ok: false, detail: `Not built yet: ${missing.map(m => m.label).join(', ')}.`,
    ...(buildable ? { fix: { action: 'build', label: 'Build missing parts', note: 'Needs Visual Studio with C++ and the Android NDK. Takes a few minutes.' } } : {}) };
}

// The PC's active OpenXR runtime (what headset play goes through), or '' when there is none.
async function activeRuntime() {
  const output = await run('reg.exe', ['query', 'HKLM\\SOFTWARE\\Khronos\\OpenXR\\1', '/v', 'ActiveRuntime'], { timeout: 10000 }).catch(() => '');
  const runtime = output.match(/ActiveRuntime\s+REG_\w+\s+(.+)/)?.[1]?.trim();
  if (!runtime) return '';
  return /oculus|meta/i.test(runtime) ? 'Meta Horizon Link' : /steam/i.test(runtime) ? 'SteamVR' : /virtual ?desktop|vdxr/i.test(runtime) ? 'Virtual Desktop' : path.basename(runtime);
}

// Only headset play needs this; games also play on the PC screen without one.
async function openxr() {
  const check = { id: 'openxr', title: 'PC VR runtime' };
  const name = await activeRuntime();
  if (!name) return { ...check, ok: false, optional: true, detail: 'No OpenXR runtime is active, so games play on this PC only. For VR, install Meta Horizon Link or SteamVR and set it as the OpenXR runtime.' };
  return { ...check, ok: true, detail: `${name} is the active OpenXR runtime.` };
}

// Whether headset play can start now: { connected, runtime, title?, detail? }. SteamVR is not probed while it
// is closed, because asking it for a headset would open it.
export async function headsetStatus(runtime) {
  const name = await activeRuntime();
  if (!name) return { connected: false, runtime: '', title: 'No PC VR software', detail: 'Install Meta Horizon Link or SteamVR to play in VR.' };
  if (name === 'SteamVR') {
    const tasks = await run('tasklist.exe', ['/FI', 'IMAGENAME eq vrserver.exe', '/NH'], { timeout: 10000 }).catch(() => '');
    if (!/vrserver\.exe/i.test(tasks)) return { connected: false, runtime: name, title: 'SteamVR is not running', detail: 'Start SteamVR with your headset connected to play in VR.' };
  }
  try { await runtime.checkHeadset(); return { connected: true, runtime: name }; }
  catch (error) {
    const detail = error.message.replace(/ and press Play again\.$/, '.');
    return { connected: false, runtime: name, title: /^No VR headset/.test(detail) ? 'No VR headset connected' : `${name} is not ready`,
      detail: /^No VR headset/.test(detail) ? (name === 'Meta Horizon Link' ? 'Put on your Quest and connect it with Quest Link (USB cable) or Air Link.' : `Connect your headset to ${name}.`) : detail };
  }
}

// What Refract Native needs instead of Android: its runner, the PC viewer, Refract's runtime libraries, Android's
// system files (from Google's stock image), and the Java and Android build tools install_apk.py converts games with.
// The release package carries both (jdk/, tools/dex2jar/); a checkout uses the developer's JDK 27 and external/dex2jar.
const nativeJdks = root => [path.join(root, 'jdk/bin/javac.exe'), process.env.JAVA_HOME && path.join(process.env.JAVA_HOME, 'bin/javac.exe'),
  'C:/Program Files/Java/jdk-27/bin/javac.exe'].filter(Boolean);
const nativeDex2jars = root => [path.join(root, 'tools/dex2jar/d2j-dex2jar.bat'), path.join(root, '../external/dex2jar/d2j-dex2jar.bat')];
const anyExists = async files => (await Promise.all(files.map(exists))).some(Boolean);
async function nativeJdk(root) {
  for (const javac of nativeJdks(root)) if (await exists(javac)) return path.dirname(path.dirname(javac));
  throw new Error('JDK 27 was not found. Open Settings > Setup.');
}
async function nativeChecks(root, sdk) {
  const parts = [
    ['Native runner', nativeExe, { action: 'native', label: 'Build Refract Native', note: 'Needs Visual Studio with C++, CMake, JDK 27 and native source dependencies. Takes several minutes.' }],
    ['ARM64 guest launcher', 'build-native/guest/bin/refract_app', { action: 'native', label: 'Build Refract Native', note: 'Needs Git Bash and Android NDK 27.3.13750724.' }],
    ['Android Java shim', 'build-native/java/refract-android.jar', { action: 'native', label: 'Build Refract Native', note: 'Needs Android platform 34, the Android system files and dex2jar.' }],
    ['PC viewer', 'viewer/build/refract_viewer.exe', { action: 'build', label: 'Build missing parts', note: 'Needs Visual Studio with C++ and the Android SDK/NDK build packages. Takes a few minutes.' }],
    ['GPU sharing layer', 'build-windows-gpu-layer/Release/refract_gpu_layer.dll', { action: 'build', label: 'Build missing parts', note: 'Needs Visual Studio with C++ and the Android NDK. Takes a few minutes.' }],
    ['Refract runtime (ARM64)', 'build-android-runtime-windows-arm64-v8a/systemdriver/package/lib/arm64-v8a/libopenxr_runtime.so', { action: 'nativeRuntime', label: 'Build runtime', note: 'Needs the Android NDK.' }],
    ['Platform stand-in (ARM64)', 'build-platform-sdk/package/lib/arm64-v8a/librefract_ovrplatform.so', { action: 'nativeRuntime', label: 'Build runtime', note: 'Needs the Android NDK.' }],
  ];
  const missing = [];
  for (const [label, file, fix] of parts) if (!await exists(path.join(root, file))) missing.push({ label, fix });
  const built = missing.length
    ? { ok: false, detail: `Not built yet: ${missing.map(m => m.label).join(', ')}.`, fix: missing[0].fix }
    : { ok: true, detail: 'Native runner, ARM64 guest launcher, Android Java shim, PC viewer, GPU sharing layer and Refract runtime are built.' };
  const sysroot = nativeSysroot(root);
  const firmware = await exists(path.join(sysroot, 'system'));
  const tools = [];
  if (!await anyExists(nativeJdks(root))) tools.push('JDK 27');
  if (!await anyExists(nativeDex2jars(root))) tools.push('dex2jar');
  if (!await exists(path.join(sdk, 'build-tools/36.1.0/aapt2.exe'))) tools.push('Android build-tools 36.1.0');
  if (!await exists(path.join(sdk, 'ndk/27.3.13750724/build/cmake/android.toolchain.cmake'))) tools.push('Android NDK 27.3.13750724');
  if (!await exists(path.join(sdk, 'platforms/android-29/android.jar'))) tools.push('Android platform 29');
  if (!await exists(path.join(sdk, 'platforms/android-34/android.jar'))) tools.push('Android platform 34');
  if (!await exists(path.join(sdk, 'cmake/3.22.1/bin/cmake.exe'))) tools.push('CMake 3.22.1');
  return [{ id: 'native', title: 'Refract Native', ...built },
    { id: 'firmware', title: 'Android system files', ok: firmware, detail: firmware ? 'Android 14 ARM64 libraries are ready.' : 'Refract Native runs games on the Android system libraries from Google ARM64 system image.',
      ...(firmware ? {} : { fix: { action: 'sysroot', label: 'Download', note: 'Downloads about 700 MB from Google. Downloading accepts the Android SDK License Agreement (developer.android.com/studio/terms).' } }) },
    { id: 'nativetools', title: 'Game conversion tools', ok: !tools.length, detail: tools.length ? `Missing: ${tools.join(', ')}.` : 'JDK, dex2jar and Android build-tools found.',
      ...(tools.includes('Android build-tools 36.1.0') || tools.includes('Android platform 34') ? { fix: { action: 'buildTools', label: 'Download', note: 'Downloads Android build tools 36.1.0 and platform 34 (about 120 MB) from Google. Downloading accepts the Android SDK License Agreement (developer.android.com/studio/terms).' } } : {}) }];
}

export async function checkSetup(root, settings) {
  if (settings.backend === 'native') {
    const [py, native, xr] = await Promise.all([python(), nativeChecks(root, settings.sdk), openxr()]);
    return [py, ...native, xr];
  }
  const [py, androidChecks, accel, parts, xr] = await Promise.all([python(), android(settings.sdk, settings.avd), hypervisor(settings.sdk), built(root, settings.sdk), openxr()]);
  return [py, ...androidChecks, accel, parts, xr];
}

export async function fixSetup(root, settings, action, progress = () => {}) {
  const long = { timeout: 60 * 60 * 1000 };
  switch (action) {
    case 'python':
      await run('winget.exe', winget('Python.Python.3.13'), long); await refreshPath(); return;
    case 'android':
      await installAndroid(settings.sdk, settings.avd, progress); return;
    case 'buildTools':
      await installBuildTools(settings.sdk, progress); return;
    case 'hypervisor': {
      // Elevated: Windows shows its permission prompt. The feature takes effect after a restart.
      const inner = 'Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All -NoRestart | Out-Null';
      const outer = `$p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile','-EncodedCommand','${Buffer.from(inner, 'utf16le').toString('base64')}'; exit $p.ExitCode`;
      await run('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', outer], long); return;
    }
    case 'sysroot': {
      await installNativeImage(settings.sdk, progress);
      progress('Preparing Android system files');
      await run('python', ['-m', 'pip', 'install', '--quiet', 'ext4'], long);
      await run('python', [path.join(root, 'native/tools/make_sysroot.py'), nativeSysroot(root)], { ...long, cwd: root });
      return;
    }
    case 'native':
      await run('cmd.exe', ['/d', '/c', path.join(root, 'native/build.cmd')], { ...long, cwd: root,
        env: { ANDROID_SDK_ROOT: settings.sdk },
        onOutput: text => { const line = text.trim().split(/\r?\n/).pop(); if (line) progress(line.slice(0, 120)); } });
      return;
    case 'nativeRuntime':
      for (const [script, parameters] of [['android-runtime-apk/build_apk.ps1', { Sdk: settings.sdk, Jdk: await nativeJdk(root), Abi: 'arm64-v8a' }], ['platform-sdk/build_apk.ps1', { Sdk: settings.sdk, Jdk: await nativeJdk(root) }]])
        await run('powershell.exe', powershellArgs(path.join(root, script), parameters), { ...long, cwd: root });
      return;
    case 'build':
      for (const item of components(root, settings.sdk, settings.backend === 'native' ? await nativeJdk(root) : undefined)) {
        if (item.build && !await exists(path.join(root, item.file))) await run(item.build[0], item.build[1], { ...long, cwd: root, ...item.build[2] });
      }
      return;
    default: throw new Error('Unknown setup step.');
  }
}
