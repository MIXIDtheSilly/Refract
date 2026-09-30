// What the Emulator page shows and does: live status, a diagnostics snapshot of the PC and Android,
// a logcat buffer the page polls, the log files Refract's scripts write, an adb shell and a diagnostics export.
import { spawn } from 'node:child_process';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { run, guestPackages, sha256 } from './runtime.mjs';
import { hypervisor } from './setup.mjs';

// Meta access tokens can show up in logs (the platform stand-in, launcher errors); keep them out of the page and exports.
export const redact = text => String(text ?? '').replace(/(?:OC|FRL|EA)[A-Za-z0-9_|-]{30,}/g, '[redacted]').replace(/access_token=[^\s&"]+/g, 'access_token=[redacted]');

const powershell = (script, timeout = 30000) => run('powershell.exe', ['-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], { timeout });
const exists = file => fs.access(file).then(() => true, () => false);

// `logcat -v threadtime`: "09-30 16:21:31.722  1234  1250 I Tag     : message".
const logcatLine = /^(\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\s+(\d+)\s+(\d+)\s+([VDIWEFS])\s+(.*?)\s*: ?(.*)$/;
export function parseLogcat(line) {
  const m = line.match(logcatLine);
  return m ? { time: m[1], pid: Number(m[2]), tid: Number(m[3]), level: m[4], tag: m[5], message: m[6] }
    : { time: '', pid: 0, tid: 0, level: '-', tag: '', message: line };
}

// A running `adb logcat` whose lines the page reads with read(after). It stops a minute after the page
// stops reading, and restarts (from the last line it has) when Android or adb comes back.
export class Logcat {
  constructor(runtime, capacity = 60000) {
    Object.assign(this, { runtime, capacity, entries: [], seq: 0, generation: 1, child: null, serial: '', error: '', lastRead: 0, lastStart: 0, paused: false });
    this.idle = setInterval(() => { if (this.child && Date.now() - this.lastRead > 60000) this.stop(); }, 15000);
    this.idle.unref();
  }
  adb() { return path.join(this.runtime.settings.sdk, 'platform-tools/adb.exe'); }
  push(entry) {
    this.entries.push([++this.seq, entry.time, entry.pid, entry.tid, entry.level, entry.tag, redact(entry.message)]);
    if (this.entries.length > this.capacity * 1.1) this.entries.splice(0, this.entries.length - this.capacity);
  }
  note(text) { this.push({ time: '', pid: 0, tid: 0, level: '-', tag: 'refract', message: text }); }
  async start() {
    if (this.child || this.starting || Date.now() - this.lastStart < 3000) return;
    this.starting = true; this.lastStart = Date.now();
    try {
      const port = await this.runtime.findPort();
      if (!port) { this.error = 'Android is not running.'; return; }
      const serial = `emulator-${port}`;
      // Continue from the last line on the same device (skipping what is already here); otherwise the recent backlog.
      const last = serial === this.serial ? this.entries.findLast(e => e[1])?.[1] : '';
      if (this.serial && serial !== this.serial) this.note(`Connected to ${serial}`);
      this.serial = serial; this.error = ''; this.skipUntil = last || '';
      const child = spawn(this.adb(), ['-s', serial, 'logcat', '-v', 'threadtime', '-T', last || '5000'], { windowsHide: true });
      this.child = child;
      let partial = '';
      child.stdout.on('data', b => {
        const lines = (partial + b.toString()).split(/\r?\n/); partial = lines.pop();
        for (const line of lines) {
          if (!line || line.startsWith('--------- beginning of')) continue;
          const entry = parseLogcat(line);
          if (this.skipUntil) { if (entry.time && entry.time <= this.skipUntil) continue; this.skipUntil = ''; }
          this.push(entry);
        }
      });
      child.stderr.on('data', b => { this.error = b.toString().trim().split(/\r?\n/).pop(); });
      child.on('error', e => { this.error = e.message; });
      child.on('exit', () => { if (this.child === child) this.child = null; });
    } finally { this.starting = false; }
  }
  stop() { this.child?.kill(); this.child = null; }
  async clear() {
    if (this.serial) await run(this.adb(), ['-s', this.serial, 'logcat', '-c'], { timeout: 10000 }).catch(() => {});
    this.entries = []; this.generation++;
  }
  // Lines after `after` (the last seq the page has); reset when the page's copy no longer lines up.
  read(after = 0, generation = 0, follow = true) {
    this.lastRead = Date.now();
    if (follow) this.start().catch(e => { this.error = e.message; });
    const reset = generation !== this.generation || (this.entries.length > 0 && after < this.entries[0][0] - 1) || after > this.seq;
    const from = reset ? 0 : after;
    let lo = 0, hi = this.entries.length;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (this.entries[mid][0] <= from) lo = mid + 1; else hi = mid; }
    return { generation: this.generation, reset, entries: this.entries.slice(lo, lo + 20000), more: this.entries.length - lo > 20000,
      running: Boolean(this.child), serial: this.serial, error: this.child ? '' : this.error, capacity: this.capacity };
  }
}

// Log files Refract's scripts and the launcher write, newest session each.
export function logSources(root, dataDirectory) {
  const emulator = path.join(root, 'build-windows-emulator'), game = path.join(root, 'build-windows-game');
  return [
    { id: 'emulator-err', label: 'Emulator (stderr)', file: path.join(emulator, 'emulator.stderr.log') },
    { id: 'emulator-out', label: 'Emulator (stdout)', file: path.join(emulator, 'emulator.stdout.log') },
    { id: 'launcher', label: 'Launcher backend', file: path.join(dataDirectory, 'launcher-backend.log') },
    { id: 'host-err', label: 'Host bridge (stderr)', file: path.join(game, 'host.err') },
    { id: 'host-out', label: 'Host bridge (stdout)', file: path.join(game, 'host.log') },
    { id: 'viewer-err', label: 'PC viewer (stderr)', file: path.join(game, 'viewer.err') },
    { id: 'viewer-out', label: 'PC viewer (stdout)', file: path.join(game, 'viewer.log') },
    { id: 'input-err', label: 'PC input server (stderr)', file: path.join(game, 'input.err') },
    { id: 'input-out', label: 'PC input server (stdout)', file: path.join(game, 'input.log') },
  ];
}
async function tail(file, bytes) {
  const handle = await fs.open(file);
  try {
    const { size } = await handle.stat();
    const start = Math.max(0, size - bytes), buffer = Buffer.alloc(size - start);
    await handle.read(buffer, 0, buffer.length, start);
    // PowerShell writes some logs as UTF-16.
    let text = buffer[0] === 0xff && buffer[1] === 0xfe || (buffer.length > 3 && buffer[1] === 0 && buffer[3] === 0) ? buffer.toString('utf16le') : buffer.toString('utf8');
    if (start) text = text.slice(text.indexOf('\n') + 1);
    return { text: redact(text.replace(/^\uFEFF/, '')), truncated: start > 0 };
  } finally { await handle.close(); }
}

const avdOf = command => command?.match(/(?:^|\s)-avd\s+"?([A-Za-z0-9_-]+)/)?.[1] || '';
const portOf = command => Number(command?.match(/(?:^|\s)-port\s+(\d+)/)?.[1] || 5554);
const section = (text, name) => text.match(new RegExp(`^@@${name}\\r?\\n([\\s\\S]*?)(?=^@@|(?![\\s\\S]))`, 'm'))?.[1].trimEnd() || '';

export class Emulator {
  constructor(runtime, root, dataDirectory) {
    Object.assign(this, { runtime, root, dataDirectory, cpu: new Map() });
    this.logcat = new Logcat(runtime);
  }
  get settings() { return this.runtime.settings; }
  adb(port, args, options) { return this.runtime.adbAt(port, args, options); }

  // Emulator, adb and Refract session processes on this PC, with CPU use since the last call.
  async processes() {
    const script = `$ErrorActionPreference = 'SilentlyContinue'
$list = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'qemu-system%' OR Name = 'emulator.exe' OR Name = 'adb.exe' OR Name = 'refract-host-bridge.exe' OR Name = 'refract_viewer.exe'" | ForEach-Object {
  $p = Get-Process -Id $_.ProcessId
  [pscustomobject]@{ pid = $_.ProcessId; parent = $_.ParentProcessId; name = $_.Name; command = $_.CommandLine; path = $_.ExecutablePath
    cpu = $(if ($p) { $p.TotalProcessorTime.TotalSeconds } else { 0 }); memory = $(if ($p) { $p.WorkingSet64 } else { 0 })
    threads = $(if ($p) { $p.Threads.Count } else { 0 }); started = $(if ($p) { $p.StartTime.ToString('o') } else { '' }) } })
ConvertTo-Json -InputObject $list -Compress -Depth 2`;
    let list = [];
    try { list = JSON.parse((await powershell(script, 20000)).trim() || '[]'); } catch { return []; }
    const now = Date.now(), threads = os.cpus().length;
    for (const p of list) {
      const before = this.cpu.get(p.pid);
      p.cpuPercent = before && now > before.at ? Math.max(0, Math.min(100, (p.cpu - before.cpu) / ((now - before.at) / 1000) / threads * 100)) : null;
      this.cpu.set(p.pid, { cpu: p.cpu, at: now });
      if (/^qemu-system/i.test(p.name)) Object.assign(p, { avd: avdOf(p.command), port: portOf(p.command), cores: Number(p.command?.match(/\s-cores\s+(\d+)/)?.[1] || 1),
        multicore: /multicore/i.test(p.path || p.command || ''), window: !/\s-no-window(\s|$)/.test(p.command || '') });
    }
    return list;
  }

  // Cheap enough to poll every few seconds while the page is open.
  async status() {
    const [port, processes] = await Promise.all([this.runtime.findPort(), this.processes()]);
    const qemu = processes.find(p => p.avd === this.settings.avd) || null;
    let booted = false, guest = null;
    if (port) {
      const text = await this.adb(port, ['shell', "getprop sys.boot_completed; cat /proc/loadavg /proc/uptime; grep -E '^(MemTotal|MemAvailable):' /proc/meminfo"], { timeout: 5000 }).catch(() => '');
      const lines = text.split(/\r?\n/);
      booted = lines[0]?.trim() === '1';
      const kb = name => Number(text.match(new RegExp(`${name}:\\s+(\\d+)`))?.[1] || 0) * 1024;
      guest = { load: lines[1]?.split(' ').slice(0, 3).map(Number) || [], uptime: Number(lines[2]?.split(' ')[0] || 0), memTotal: kb('MemTotal'), memAvailable: kb('MemAvailable') };
    }
    return {
      state: port ? (booted ? 'running' : 'booting') : qemu ? 'starting' : 'stopped',
      port, serial: port ? `emulator-${port}` : '', configuredPort: this.settings.port, avd: this.settings.avd,
      adopted: Boolean(port && port !== this.settings.port), startedHere: Boolean(this.runtime.started), game: this.runtime.game,
      qemu, guest, processes: processes.map(({ command, ...p }) => ({ ...p, command: redact(command) })), logcat: { running: Boolean(this.logcat.child), lines: this.logcat.entries.length },
    };
  }

  // A full snapshot for the Overview tab and diagnostics exports.
  async info() {
    const port = await this.runtime.findPort();
    const [host, guest, components] = await Promise.all([this.hostInfo(), port ? this.guestInfo(port) : null, this.components(port)]);
    return { collected: new Date().toISOString(), port, serial: port ? `emulator-${port}` : '', host, guest, components };
  }

  async hostInfo() {
    const script = `$ErrorActionPreference = 'SilentlyContinue'
$os = Get-CimInstance Win32_OperatingSystem; $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$xr = (Get-ItemProperty 'HKLM:\\SOFTWARE\\Khronos\\OpenXR\\1').ActiveRuntime
[pscustomobject]@{ os = "$($os.Caption) $($os.Version)"; memory = [int64]$os.TotalVisibleMemorySize * 1024; free = [int64]$os.FreePhysicalMemory * 1024
  cpu = $cpu.Name.Trim(); cores = $cpu.NumberOfCores; threads = $cpu.NumberOfLogicalProcessors
  gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" })
  openxr = $xr } | ConvertTo-Json -Compress -Depth 3`;
    const sdk = this.settings.sdk;
    const [system, emulatorVersion, adbVersion, commit, acceleration] = await Promise.all([
      powershell(script).then(t => JSON.parse(t.trim()), () => ({})),
      fs.readFile(path.join(sdk, 'emulator/source.properties'), 'utf8').then(t => t.match(/Pkg\.Revision=(.+)/)?.[1].trim() || '', () => ''),
      run(path.join(sdk, 'platform-tools/adb.exe'), ['version'], { timeout: 5000 }).then(t => t.split(/\r?\n/).slice(0, 2).join(' · '), () => ''),
      run('git', ['-C', this.root, 'log', '-1', '--format=%h %cs %s'], { timeout: 5000 }).then(t => t.trim(), () => ''),
      hypervisor(sdk).then(c => ({ ok: c.ok, detail: c.detail }), () => null),
    ]);
    const multicore = await exists(path.join(sdk, 'emulator/qemu/windows-x86_64/qemu-system-x86_64-multicore.exe'));
    const launcherVersion = await fs.readFile(path.join(this.root, 'launcher/package.json'), 'utf8').then(t => JSON.parse(t).version, () => '');
    return { ...system, acceleration, sdk, emulatorVersion, adbVersion, multicoreQemu: multicore, node: process.version, launcherVersion, commit, root: this.root, data: this.dataDirectory };
  }

  async guestInfo(port) {
    const packages = guestPackages.map(p => p.package).join(' ');
    // One adb round trip. No double quotes: the script passes through Windows argument quoting.
    const script = [
      'echo @@props', 'getprop',
      'echo @@meminfo', 'cat /proc/meminfo',
      'echo @@cpu', 'nproc', 'cat /proc/loadavg', 'cat /proc/uptime',
      // Readable only as root (the emulator's userdebug build has su), like the start script's clock check.
      'echo @@clock', 'su 0 cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null',
      'echo @@kernel', 'uname -a',
      'echo @@cmdline', 'su 0 cat /proc/cmdline 2>/dev/null',
      'echo @@selinux', 'getenforce',
      'echo @@display', 'wm size', 'wm density',
      'echo @@gles', "dumpsys SurfaceFlinger | grep -m1 '^GLES'",
      'echo @@df', 'df -h /data /sdcard /system 2>/dev/null',
      'echo @@settings', 'settings get secure immersive_mode_confirmations', 'settings get global hide_error_dialogs',
      'echo @@focus', 'dumpsys window 2>/dev/null | grep -m1 mCurrentFocus',
      'echo @@guest', `for p in ${packages}; do f=$(pm path $p 2>/dev/null | grep -m1 base.apk | cut -d: -f2); if [ -z $f ]; then echo $p -; else echo $p $(sha256sum $f | cut -d' ' -f1) $(dumpsys package $p | grep -m1 versionName | tr -d ' '); fi; done`,
      'echo @@packages', 'pm list packages -3 --show-versioncode',
      'echo @@top', 'top -b -n 1 -m 15',
      'echo @@end',
    ].join('; ');
    const text = (await this.adb(port, ['shell', script], { timeout: 30000 })).replace(/\r\n/g, '\n');
    const props = Object.fromEntries([...section(text, 'props').matchAll(/^\[([^\]]+)\]: \[([\s\S]*?)\]$/gm)].map(m => [m[1], m[2]]));
    const kb = name => Number(section(text, 'meminfo').match(new RegExp(`^${name}:\\s+(\\d+)`, 'm'))?.[1] || 0) * 1024;
    const [cores, loadavg, uptime] = section(text, 'cpu').split('\n');
    const guest = section(text, 'guest').split(/\r?\n/).filter(Boolean).map(line => {
      const [name, hash, version] = line.trim().split(/\s+/);
      return { package: name, installed: hash !== '-', sha256: hash === '-' ? '' : hash, version: (version || '').replace(/^versionName=/, '') };
    });
    return {
      props, clocksource: section(text, 'clock').trim(), cores: Number(cores) || 0, loadavg: loadavg?.trim() || '', uptime: Number(uptime?.split(' ')[0] || 0),
      memory: { total: kb('MemTotal'), available: kb('MemAvailable'), swapTotal: kb('SwapTotal'), swapFree: kb('SwapFree') },
      kernel: section(text, 'kernel').trim(), cmdline: section(text, 'cmdline').trim(), selinux: section(text, 'selinux').trim(), display: section(text, 'display').split(/\r?\n/).map(s => s.trim()).filter(Boolean).join(' · '),
      gles: section(text, 'gles').replace(/^GLES:\s*/, '').trim(), storage: section(text, 'df'), immersiveConfirmed: section(text, 'settings').split('\n')[0]?.trim() === 'confirmed', errorDialogsHidden: section(text, 'settings').split('\n')[1]?.trim() === '1',
      focus: section(text, 'focus').match(/u0 ([^}]+)\}/)?.[1].trim() || section(text, 'focus').trim(),
      guestPackages: guest,
      packages: section(text, 'packages').split(/\r?\n/).map(l => l.match(/^package:(\S+)(?:\s+versionCode:(\d+))?/)).filter(Boolean).map(m => ({ package: m[1], versionCode: m[2] || '' })).sort((a, b) => a.package.localeCompare(b.package)),
      top: section(text, 'top'),
    };
  }

  // Refract's build outputs on this PC; with Android running, whether the guest has the same APKs.
  async components() {
    const files = [
      { label: 'Host bridge', file: 'build-windows-nvidia/host-bridge/refract-host-bridge.exe' },
      { label: 'PC viewer', file: 'viewer/build/refract_viewer.exe' },
      { label: 'GPU sharing layer', file: 'build-windows-gpu-layer/Release/refract_gpu_layer.json' },
      ...guestPackages.map(p => ({ label: p.label, file: p.apk, package: p.package })),
    ];
    return Promise.all(files.map(async item => {
      const full = path.join(this.root, item.file);
      const stat = await fs.stat(full).catch(() => null);
      return { ...item, path: full, built: Boolean(stat), modified: stat?.mtime.toISOString() || '', size: stat?.size || 0,
        sha256: stat && item.package ? await sha256(full).catch(() => '') : '' };
    }));
  }

  sources() { return logSources(this.root, this.dataDirectory); }
  async logFile(id) {
    const source = this.sources().find(s => s.id === id);
    if (!source) throw new Error('Unknown log file.');
    const stat = await fs.stat(source.file).catch(() => null);
    if (!stat) return { ...source, exists: false, text: '', size: 0, modified: '' };
    return { ...source, exists: true, size: stat.size, modified: stat.mtime.toISOString(), ...await tail(source.file, 4 * 1024 * 1024) };
  }
  async logFiles() {
    return Promise.all(this.sources().map(async s => { const stat = await fs.stat(s.file).catch(() => null); return { ...s, exists: Boolean(stat), size: stat?.size || 0, modified: stat?.mtime.toISOString() || '' }; }));
  }

  // One command in Android's shell, for the Shell tab. Output and exit code, never an exception for a failing command.
  async shell(command) {
    if (typeof command !== 'string' || !command.trim() || command.length > 8000) throw new Error('Enter a shell command.');
    const port = await this.runtime.findPort();
    if (!port) throw new Error('Android is not running.');
    const started = Date.now();
    return new Promise(resolve => {
      const child = spawn(path.join(this.settings.sdk, 'platform-tools/adb.exe'), ['-s', `emulator-${port}`, 'shell', command], { windowsHide: true });
      let output = '', truncated = false;
      const add = b => { if (output.length < 1024 * 1024) output += b.toString(); else truncated = true; };
      child.stdout.on('data', add); child.stderr.on('data', add);
      const timer = setTimeout(() => { truncated = true; output += '\n[stopped after 60 seconds]'; child.kill(); }, 60000);
      child.on('error', e => { clearTimeout(timer); resolve({ code: -1, output: e.message, ms: Date.now() - started }); });
      child.on('close', code => { clearTimeout(timer); resolve({ code, output: redact(output.replace(/\r\n/g, '\n')), truncated, ms: Date.now() - started, serial: `emulator-${port}` }); });
    });
  }

  async screenshot() {
    const port = await this.runtime.findPort();
    if (!port) throw new Error('Android is not running.');
    const directory = path.join(this.dataDirectory, 'screenshots');
    await fs.mkdir(directory, { recursive: true });
    const file = path.join(directory, `android-${new Date().toISOString().replace(/[:.]/g, '-')}.png`);
    await this.adb(port, ['shell', 'screencap -p /data/local/tmp/refract-screen.png'], { timeout: 20000 });
    await this.adb(port, ['pull', '/data/local/tmp/refract-screen.png', file], { timeout: 20000 });
    return { path: file, image: `data:image/png;base64,${(await fs.readFile(file)).toString('base64')}` };
  }

  // A folder and zip with everything needed to look into someone's problem: the snapshot, full logcat,
  // every log file and the launcher settings (never the Meta sign-in or library).
  async exportDiagnostics(extra = {}) {
    const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    const base = path.join(this.dataDirectory, 'diagnostics'), directory = path.join(base, `refract-diagnostics-${stamp}`);
    await fs.mkdir(path.join(directory, 'logs'), { recursive: true });
    const [info, status] = await Promise.all([this.info().catch(e => ({ error: e.message })), this.status().catch(e => ({ error: e.message }))]);
    await fs.writeFile(path.join(directory, 'diagnostics.json'), redact(JSON.stringify({ ...extra, status, info }, null, 2)));
    if (info.guest?.top) await fs.writeFile(path.join(directory, 'top.txt'), info.guest.top);
    if (info.port) {
      const logcat = await this.adb(info.port, ['logcat', '-d', '-v', 'threadtime'], { timeout: 60000 }).catch(e => `logcat failed: ${e.message}`);
      await fs.writeFile(path.join(directory, 'logcat.txt'), redact(logcat));
      const props = await this.adb(info.port, ['shell', 'getprop'], { timeout: 10000 }).catch(() => '');
      await fs.writeFile(path.join(directory, 'getprop.txt'), props);
    } else if (this.logcat.entries.length) {
      await fs.writeFile(path.join(directory, 'logcat-launcher-buffer.txt'), this.logcat.entries.map(formatEntry).join('\n'));
    }
    for (const source of this.sources()) {
      if (!await exists(source.file)) continue;
      const { text } = await tail(source.file, 16 * 1024 * 1024);
      await fs.writeFile(path.join(directory, 'logs', path.basename(source.file)), text);
    }
    const zip = `${directory}.zip`;
    // '/' entry names (PS 5.1's ZipFile writes '\' otherwise), so the zip opens anywhere.
    await powershell(`[AppContext]::SetSwitch('Switch.System.IO.Compression.ZipFile.UseBackslash', $false); Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::CreateFromDirectory('${directory.replaceAll("'", "''")}', '${zip.replaceAll("'", "''")}')`, 120000).catch(() => {});
    return { directory, zip: await exists(zip) ? zip : '', base };
  }
}

export const formatEntry = e => e[1] ? `${e[1]} ${String(e[2]).padStart(5)} ${String(e[3]).padStart(5)} ${e[4]} ${e[5]}: ${e[6]}` : e[6];
