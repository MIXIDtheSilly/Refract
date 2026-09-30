import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import fs from 'node:fs/promises';
import path from 'node:path';

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// What a failed tool printed, as one readable message. PowerShell started without a console
// writes its error stream as CLIXML; powershellArgs() adds a plain REFRACT-ERROR line instead.
export function readableError(text) {
  text = String(text || '');
  const marked = [...text.matchAll(/^REFRACT-ERROR: (.+)$/gm)].pop();
  if (marked) return marked[1].trim();
  if (/#< CLIXML|<\/Objs>/.test(text)) {
    const decode = s => s.replace(/_x([0-9A-F]{4})_/gi, (_, h) => String.fromCharCode(parseInt(h, 16)))
      .replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&amp;/g, '&');
    text = [...text.matchAll(/<S S="Error">([\s\S]*?)<\/S>/g)].map(m => decode(m[1])).join('')
      // A tail can start inside a CLIXML block; drop that fragment too.
      + text.replace(/#< CLIXML[\s\S]*?(?:<\/Objs>|$)/g, '').replace(/^[\s\S]*?<\/Objs>/, '');
  }
  // Drop PowerShell's position report ("At C:\...ps1:108 char:35", "+ ~~~", "+ CategoryInfo ...").
  const lines = text.split(/\r?\n/).map(l => l.trimEnd())
    .filter(l => l.trim() && !/^\s*(At (line:\d+ )?[A-Za-z]:\\.*char:\d+|At line:\d+ char:\d+|\+ |\+$)/.test(l) && !/^\s*\+?\s*(CategoryInfo|FullyQualifiedErrorId)\s*:/.test(l));
  return lines.slice(-12).join('\n').slice(-3000);
}

// Why the host bridge could not reach the headset, from the OpenXR error it printed, in words a player can act on.
export function headsetProblem(text) {
  if (/XR_ERROR_FORM_FACTOR_UNAVAILABLE/.test(text)) return 'No VR headset is connected. Connect your headset (Meta Horizon Link or Air Link, or SteamVR) and press Play again.';
  if (/XR_ERROR_RUNTIME_UNAVAILABLE|XR_ERROR_RUNTIME_FAILURE|xrCreateInstance|openxr_loader/.test(text)) return 'The PC VR runtime is not running. Start Meta Horizon Link or SteamVR and press Play again.';
  return '';
}

export function run(executable, args, { timeout = 120000, onOutput = () => {}, cwd } = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(executable, args, { windowsHide: true, shell: false, cwd });
    let output = '', errors = '', settled = false;
    const timer = setTimeout(() => { child.kill(); finish(new Error('Operation timed out. Check the Android runtime and try again.')); }, timeout);
    function finish(error) { if (settled) return; settled = true; clearTimeout(timer); error ? reject(error) : resolve(output); }
    child.stdout.on('data', b => { output = (output + b.toString()).slice(-8 * 1024 * 1024); onOutput(b.toString()); });
    child.stderr.on('data', b => { errors = (errors + b.toString()).slice(-16384); onOutput(b.toString()); });
    child.on('error', e => finish(new Error(e.code === 'ENOENT' ? `${path.basename(executable)} was not found. Open Settings > Setup.` : `Could not start ${path.basename(executable)}: ${e.code || 'unknown error'}`)));
    child.on('exit', code => finish(code === 0 ? null : new Error(readableError(`${output.slice(-16384)}\n${errors}`) || `${path.basename(executable)} exited with code ${code}`)));
  });
}
const psLiteral = value => `'${String(value).replaceAll("'", "''")}'`;
export function powershellArgs(script, parameters) {
  const invocation = `$ProgressPreference = 'SilentlyContinue'; try { & ${psLiteral(script)} ${Object.entries(parameters).map(([key, value]) => {
    if (!/^[a-zA-Z]+$/.test(key)) throw new Error('Invalid PowerShell parameter.');
    return value === true ? `-${key}:$true` : `-${key} ${psLiteral(value)}`;
  }).join(' ')}; if (-not $?) { exit 1 } } catch { [Console]::Error.WriteLine('REFRACT-ERROR: ' + $_.Exception.Message); exit 1 }`;
  return ['-NoProfile', '-NonInteractive', '-OutputFormat', 'Text', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', Buffer.from(invocation, 'utf16le').toString('base64')];
}
const hashes = new Map();
export async function sha256(file) {
  const stat = await fs.stat(file), key = `${file}|${stat.size}|${stat.mtimeMs}`;
  if (!hashes.has(key)) hashes.set(key, await new Promise((resolve, reject) => {
    const hash = createHash('sha256');
    createReadStream(file).on('data', b => hash.update(b)).on('error', reject).on('end', () => resolve(hash.digest('hex')));
  }));
  return hashes.get(key);
}
// Refract's Android side: the OpenXR runtime games render through, the same runtime as com.oculus.systemdriver
// (where Meta's OpenXR loader in many Unity games looks for it), and the Meta Platform SDK stand-in
// (package com.oculus.horizon) that unmodified Quest APKs load instead of Horizon OS.
export const guestPackages = [
  { package: 'com.refract.openxrruntime', label: 'Refract OpenXR runtime', apk: 'build-android-runtime-windows-arm64-v8a/refract-openxr-runtime-debug.apk' },
  { package: 'com.oculus.systemdriver', label: 'Refract XR driver', apk: 'build-android-runtime-windows-arm64-v8a/refract-systemdriver-debug.apk' },
  { package: 'com.oculus.horizon', label: 'Meta Platform stand-in', apk: 'build-platform-sdk/refract-platform-debug.apk' },
];
// Emulator page settings, as tools/windows_android_emulator.ps1 parameters. Switches are only passed when on.
export const audioBackends = ['dsound', 'winaudio', 'sdl'];
export function emulatorOptions(settings) {
  return { Cores: settings.cores ?? 6, Audio: settings.audio || 'dsound',
    ...(settings.showWindow ? { ShowWindow: true } : {}), ...(settings.hostMic === false ? { NoHostMic: true } : {}) };
}
export function validPackage(value) {
  if (!/^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+$/.test(value || '')) throw new Error('Invalid Android package name.');
  return value;
}
export class Runtime {
  constructor(root, settings) { this.root = root; this.settings = settings; this.child = null; this.game = null; this.found = null; }
  // The emulator in use: the configured port, or another one running the same AVD (see locate()).
  get port() { return this.found ?? this.settings.port; }
  adbAt(port, args, options) { return run(path.join(this.settings.sdk, 'platform-tools/adb.exe'), [...(port ? ['-s', `emulator-${port}`] : []), ...args], options); }
  adb(args, options) { return this.adbAt(this.port, args, options); }
  async online() { return Boolean(await this.locate()); }
  async avdName(port) { return (await this.adbAt(port, ['emu', 'avd', 'name'], { timeout: 5000 })).split(/\r?\n/)[0].trim(); }
  // The emulator locks its AVD, so while the same AVD runs on another port (scripts\start_emulator.ps1
  // uses 5582) a copy on the configured port exits at once. Use the running one instead.
  async locate() {
    const port = await this.findPort();
    this.found = port && port !== this.settings.port ? port : null;
    return port;
  }
  // locate() without remembering the result, for status checks that run beside a starting game.
  async findPort() {
    const ready = async port => { try { return (await this.adbAt(port, ['get-state'], { timeout: 2500 })).trim() === 'device'; } catch { return false; } };
    if (await ready(this.settings.port)) return this.settings.port;
    let devices = '';
    try { devices = await this.adbAt(null, ['devices'], { timeout: 5000 }); } catch { return null; }
    for (const [, port] of devices.matchAll(/^emulator-(\d+)\s+device\s*$/gm)) {
      if (await this.avdName(Number(port)).catch(() => '') === this.settings.avd) return Number(port);
    }
    return null;
  }
  // Console port of an emulator process already running the configured AVD, even one still booting
  // (it is not an adb "device" yet, but it already holds the AVD lock).
  async avdProcessPort() {
    let list = '';
    try {
      list = await run('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command',
        "Get-CimInstance Win32_Process -Filter \"Name LIKE 'qemu-system%' OR Name = 'emulator.exe'\" | ForEach-Object { $_.CommandLine }"], { timeout: 20000 });
    } catch { return null; }
    for (const line of list.split(/\r?\n/)) {
      const avd = line.match(/(?:^|\s)-avd\s+"?([A-Za-z0-9_-]+)/)?.[1];
      if (avd === this.settings.avd) return Number(line.match(/(?:^|\s)-port\s+(\d+)/)?.[1] || 5554);
    }
    return null;
  }
  async waitBoot(port, timeout = 240000) {
    for (const end = Date.now() + timeout; Date.now() < end; await sleep(2000)) {
      if ((await this.adbAt(port, ['shell', 'getprop', 'sys.boot_completed'], { timeout: 5000 }).catch(() => '')).trim() === '1') return;
    }
    throw new Error(`Android on emulator-${port} did not finish starting. Close the emulator and try again.`);
  }
  // Asks the PC VR runtime for a headset the way a session does, so Play fails in a second instead of after Android starts.
  async checkHeadset() {
    try { await run(path.join(this.root, 'build-windows-nvidia/host-bridge/refract-host-bridge.exe'), ['--probe-openxr'], { timeout: 60000 }); }
    catch (error) {
      if (/Usage:/.test(error.message)) return;  // A bridge built before --probe-openxr; the session reports problems instead.
      if (/timed out/.test(error.message)) throw new Error('The PC VR runtime did not respond. Restart Meta Horizon Link or SteamVR and press Play again.');
      throw new Error(headsetProblem(error.message) || error.message);
    }
  }
  async ensure(update = () => {}) {
    let port = await this.locate();
    if (port === this.settings.port) {
      const name = await this.avdName(port);
      if (name !== this.settings.avd) throw new Error(`Port ${port} is used by another virtual device (${name}). Close it or choose another port in Settings.`);
    }
    if (!port && (port = await this.avdProcessPort())) {
      update('Waiting for Android to start');
      this.found = port === this.settings.port ? null : port;
    }
    if (port) await this.waitBoot(port);
    else {
      this.started = true;
      await run('powershell.exe', powershellArgs(path.join(this.root, 'tools/windows_android_emulator.ps1'), {
        Action: 'Start', Avd: this.settings.avd, Port: this.settings.port, Sdk: this.settings.sdk,
        // No clock correction: it only works with emulator 36.5.11 and runs a single vCPU; the start
        // script's tsc=nowatchdog and TSC reboots keep the guest on the TSC clock instead.
        Abi: 'arm64-v8a', MemoryMB: this.settings.memoryMB, GpuSharing: true, ...emulatorOptions(this.settings)
      }), { timeout: 25 * 60 * 1000, onOutput: text => {  // Room for a new AVD's first boot and the TSC reboots.
        for (const [, stage] of text.matchAll(/^REFRACT-STAGE: (.+)$/gm)) update(stage.trim());
      } });
    }
    await this.prepare(update);
  }
  // Brings the running Android up to date with this Refract checkout. The emulator start script
  // installs the translator itself; an emulator started some other way may still need it.
  async prepare(update = () => {}) {
    if ((await this.adb(['shell', 'getprop', 'ro.dalvik.vm.native.bridge'])).trim() !== 'libberberis_arm64.so') {
      update('Installing the ARM translator (Android restarts)');
      await run('powershell.exe', powershellArgs(path.join(this.root, 'scripts/translator.ps1'), {
        Use: 'digitalis', Serial: `emulator-${this.port}`, Adb: path.join(this.settings.sdk, 'platform-tools/adb.exe') }), { timeout: 300000 });
      await sleep(5000); await this.waitBoot(this.port);
    }
    // Unmodified Quest games only use the Meta Platform SDK (Refract's stand-in) when Build.MANUFACTURER says
    // Oculus (Unity's getIsOnOculusHardware), so the device presents itself as a Quest 3 (scripts/device_identity.ps1).
    if (!/oculus/i.test(await this.adb(['shell', 'getprop', 'ro.product.manufacturer']))) {
      update('Setting up the Quest identity (Android restarts)');
      await this.adb(['root'], { timeout: 30000 }); await sleep(2000); await this.adb(['wait-for-device'], { timeout: 60000 });
      await this.adb(['remount'], { timeout: 60000 });
      const files = '/system/build.prop /vendor/build.prop /product/etc/build.prop /system_ext/etc/build.prop /odm/etc/build.prop';
      await this.adb(['shell', `for f in ${files}; do [ -f $f ] || continue; [ -f $f.before-identity ] || cp -p $f $f.before-identity; `
        + "sed -i -E 's/^(ro\\.product\\.([a-z_]+\\.)?brand)=.*/\\1=oculus/; s/^(ro\\.product\\.([a-z_]+\\.)?manufacturer)=.*/\\1=Oculus/; "
        + "s/^(ro\\.product\\.([a-z_]+\\.)?model)=.*/\\1=Quest 3/' $f; done; sync"]);
      await this.adb(['reboot'], { timeout: 30000 }).catch(() => {});
      await sleep(5000); await this.waitBoot(this.port);
      if (!/oculus/i.test(await this.adb(['shell', 'getprop', 'ro.product.manufacturer']))) throw new Error('Android did not take the Quest identity. Restart Refract and try again.');
    }
    // A new Android shows a one-time "Viewing full screen" notice over the first full-screen app. It takes focus,
    // so a Unity game pauses itself and never renders a frame. Mark the notice as already seen.
    if ((await this.adb(['shell', 'settings', 'get', 'secure', 'immersive_mode_confirmations'])).trim() !== 'confirmed') {
      await this.adb(['shell', 'settings', 'put', 'secure', 'immersive_mode_confirmations', 'confirmed']);
    }
    // System apps a fresh Android still has (safetyhub, switchaccess, systemui under translation load) sometimes crash
    // or stop responding at boot. Their "Application Error"/"not responding" dialog takes focus, the game never
    // becomes the resumed activity and stays black with no UI. Nobody can tap those dialogs here, so don't show them.
    if ((await this.adb(['shell', 'settings', 'get', 'global', 'hide_error_dialogs'])).trim() !== '1') {
      await this.adb(['shell', 'settings', 'put', 'global', 'hide_error_dialogs', '1']);
    }
    for (const item of guestPackages) {
      const apk = path.join(this.root, item.apk);
      const local = await sha256(apk).catch(() => { throw new Error(`${item.label} is not built (${item.apk}). Open Settings > Setup.`); });
      const installed = (await this.adb(['shell', 'pm', 'path', item.package]).catch(() => '')).match(/^package:(\S*base\.apk)/m)?.[1];
      const remote = installed && (await this.adb(['shell', 'sha256sum', installed]).catch(() => '')).split(/\s/)[0];
      if (remote === local) continue;
      update(`Installing ${item.label}`);
      await this.adb(['install', '--no-incremental', '--force-queryable', '-r', apk], { timeout: 240000 });
    }
  }
  async inspect(apk) {
    const data = JSON.parse(await run('python', [path.join(this.root, 'launcher/inspect_apk.py'), '--apk', apk, '--sdk', this.settings.sdk]));
    validPackage(data.package);
    return { ...data, apk: path.resolve(apk), source: 'local', id: `local:${data.package}` };
  }
  async installed() {
    if (!await this.online()) return null;
    return new Set((await this.adb(['shell', 'pm', 'list', 'packages', '-3'])).split(/\r?\n/).map(s => s.replace(/^package:/, '').trim()).filter(Boolean));
  }
  async importInstalled(packageName, imagePath) {
    validPackage(packageName);
    const activity = (await this.adb(['shell', 'cmd', 'package', 'resolve-activity', '--brief', packageName])).split(/\r?\n/).find(s => s.startsWith(`${packageName}/`));
    if (!activity) return null;
    const metadata = JSON.parse(await run('python', [path.join(this.root, 'tools/android_app_label.py'), '--sdk', this.settings.sdk,
      '--serial', `emulator-${this.port}`, '--package', packageName, '--icon-output', imagePath]));
    return { id: `local:${packageName}`, package: packageName, activity, name: metadata.label, source: 'installed', installed: true,
      image: metadata.icon ? `data:image/png;base64,${(await fs.readFile(metadata.icon)).toString('base64')}` : '' };
  }
  async install(game, update = () => {}) {
    validPackage(game.package);
    if (this.child) throw new Error('Close the running game before installing.');
    if (!game.apk) throw new Error('Import or download an APK first.');
    update('Starting Android'); await this.ensure(update);
    update('Installing APK');
    // -g grants the runtime permissions (microphone, notifications) up front: the hidden emulator's permission
    // dialog takes focus, the game stops responding behind it and Android closes it (Yeeps' microphone request).
    await this.adb(['install', '--no-incremental', '--force-queryable', '-r', '-g', game.apk], { timeout: 240000 });
    for (const file of game.files || []) {
      if (file.kind === 'apk') continue;
      update(`Copying ${file.name}`);
      // Preserve the filename supplied by Meta for expansion/asset delivery.
      const directory = `/sdcard/Android/obb/${game.package}`;
      await this.adb(['shell', 'mkdir', '-p', directory]);
      // shell mkdir only contains the validated package, not server filenames.
      await this.adb(['push', file.path, `${directory}/${file.name}`], { timeout: 30 * 60 * 1000 });
    }
    await this.adb(['shell', 'sync']);
  }
  // mode 'vr' shows the game in the headset (host bridge); 'pc' in a window on this PC (refract_viewer).
  launch(game, onExit, mode = 'vr') {
    if (this.child) throw new Error('A game is already running.');
    validPackage(game.package);
    if (!/^[A-Za-z0-9_./]+$/.test(game.activity || '') || game.activity.split('/')[0] !== game.package) throw new Error('Invalid launch activity. Refresh installed games.');
    const args = powershellArgs(path.join(this.root, 'tools/run_windows_game.ps1'), { Avd: this.settings.avd, Port: this.port,
      Sdk: this.settings.sdk, MemoryMB: this.settings.memoryMB, Package: game.package, Activity: game.activity, GameName: game.name,
      ...(game.owned ? { Owned: true } : {}), ...(mode === 'pc' ? { PcViewer: true } : {}) });
    // Windows PowerShell can exit successfully without executing its command
    // when CREATE_NEW_PROCESS_GROUP/detached is combined with no console.
    const child = spawn('powershell.exe', args, { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    this.child = child; this.game = game.id; this.mode = mode;
    // The script's REFRACT-ERROR line is kept apart from the tail: PowerShell writes its CLIXML log after it.
    let tail = '', marked = '', partial = '';
    child.stdout.on('data', b => { tail = (tail + b).slice(-4000); });
    child.stderr.on('data', b => {
      tail = (tail + b).slice(-4000);
      const lines = (partial + b).split(/\r?\n/); partial = lines.pop().slice(-4000);
      for (const line of lines) if (line.startsWith('REFRACT-ERROR: ')) marked = line.slice(15).trim();
    });
    let finished = false;
    const end = (code, error) => {
      if (finished) return; finished = true; this.child = null; this.game = null; this.mode = null;
      // A session the player stopped ended as asked, whatever its window's exit code.
      if (this.stopping) { this.stopping = false; code = 0; error = ''; }
      if (partial.startsWith('REFRACT-ERROR: ')) marked = partial.slice(15).trim();
      onExit(code, error || (code ? marked || readableError(tail) || 'The game session stopped unexpectedly.' : tail));
    };
    child.on('error', e => end(1, e.message)); child.on('exit', code => end(code)); child.unref();
  }
  // The emulator runs hidden, so one the launcher started stops with the launcher (a running game keeps it).
  async shutdown() {
    if (!this.started || this.child || this.settings.keepEmulator) return;
    await this.adbAt(this.settings.port, ['shell', 'sync'], { timeout: 2000 }).catch(() => {});
    await this.adbAt(this.settings.port, ['emu', 'kill'], { timeout: 2000 }).catch(() => {});
  }
  async stop() {
    if (!this.child) return;
    const pid = this.child.pid;
    this.stopping = true;
    // Closing the session's window (headset bridge or PC viewer) ends the session through its save-aware shutdown.
    const script = `$p = Get-CimInstance Win32_Process -Filter "Name='refract-host-bridge.exe' OR Name='refract_viewer.exe'" | Where-Object ParentProcessId -eq ${Number(pid)}; foreach ($h in $p) { $null = (Get-Process -Id $h.ProcessId).CloseMainWindow() }`;
    await run('powershell.exe', ['-NoProfile', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')]);
  }
}
