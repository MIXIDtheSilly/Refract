// What the Emulator page shows and does: live status, a diagnostics snapshot of the PC and Android,
// a logcat buffer the page polls, the log files Refract's scripts write, an adb shell and a diagnostics export.
import { spawn } from 'node:child_process';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { run, guestPackages, sha256, validPackage } from './runtime.mjs';
import { hypervisor } from './setup.mjs';
import { avdHome } from './android_sdk.mjs';

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

// Where Android's space goes, measured as root in one pass (about a second). Sizes are KB from du.
// Per app: APK folder (with its libraries and compiled code), internal data, Android/data, cache, OBB game files.
const shared = '/data/media/0', temp = '/data/local/tmp';
const storageScript = `s() { du -sk "$@" 2>/dev/null | awk '{n+=$1} END {print n+0}'; }
m=${shared}
echo @@df; df -k /data | tail -n 1
echo @@all; pm list packages | cut -d: -f2
echo @@apps
pm list packages -3 -f | while IFS= read -r l; do
  l=\${l#package:}; p=\${l##*=}; a=\${l%=*}
  echo "$p $(s \${a%/*}) $(s /data/data/$p /data/user_de/0/$p) $(s $m/Android/data/$p) $(s /data/data/$p/cache /data/data/$p/code_cache /data/user_de/0/$p/cache /data/user_de/0/$p/code_cache $m/Android/data/$p/cache) $(s $m/Android/obb/$p)"
done
echo @@folders
for d in $m/Android/obb/* $m/Android/data/*; do [ -d "$d" ] && echo "$(s "$d")	$d"; done
echo @@temp
for f in ${temp}/* ${temp}/.[!.]*; do [ -e "$f" ] && echo "$(s "$f")	$f"; done
echo @@shared
for f in $m/*; do [ -e "$f" ] && [ "$f" != $m/Android ] && echo "$(s "$f")	$f"; done
echo @@end
`;
const sizeLines = text => text.split('\n').map(line => line.match(/^(\d+)\t(.+)$/)).filter(Boolean).map(m => ({ size: Number(m[1]) * 1024, path: m[2] }));
// Paths the Storage tab may delete: a leftover game folder of an app that is no longer installed, or anything in /data/local/tmp.
const leftoverFolder = new RegExp(`^${shared}/Android/(obb|data)/([^/]+)$`);
const tempFile = /^\/data\/local\/tmp\/(?!\.\.?$)[^/]+$/;
const shellQuote = value => `'${value.replaceAll("'", "'\\''")}'`;
// Virtual disk sizes Reset Android and Make the disk bigger offer (disk.dataPartition.size).
export const diskSizes = [16, 32, 48, 64, 96, 128, 192, 256];
// Growing /data keeps everything on it. vold runs /system/bin/e2fsck on the decrypted /data right before mounting it,
// the only moment it is unmounted, so for one boot this wrapper takes e2fsck's place and runs resize2fs there.
// (The kernel's online resize stops at the first backup group: "reserved block 512 not at offset 511".)
// tools/windows_android_emulator.ps1 boots it with SELinux permissive, then puts the real e2fsck back.
const growWrapper = `#!/system/bin/sh
# Refract: for one boot only (see launcher/core/emulator.mjs). The real e2fsck is e2fsck.real.
for last; do :; done
/system/bin/e2fsck.real "$@"
result=$?
if [ "$(readlink -f /dev/block/mapper/userdata)" = "$(readlink -f "$last")" ] && [ $result -le 1 ]; then
  echo "refract-grow: resizing $last" > /dev/kmsg
  /system/bin/e2fsck.real -f -y "$last" > /metadata/refract-grow.log 2>&1
  /system/bin/resize2fs "$last" >> /metadata/refract-grow.log 2>&1
  echo "refract-grow: resize2fs exit $?" > /dev/kmsg
fi
exit $result
`;
function parseSize(text) {
  const m = String(text || '').trim().match(/^(\d+(?:\.\d+)?)\s*([KMGT]?)B?$/i);
  return m ? Math.round(Number(m[1]) * 1024 ** ' KMGT'.indexOf((m[2] || ' ').toUpperCase())) : 0;
}
async function folderSize(target) {
  const stat = await fs.lstat(target).catch(() => null);
  if (!stat?.isDirectory()) return stat?.size || 0;
  let total = 0;
  for (const entry of await fs.readdir(target).catch(() => [])) total += await folderSize(path.join(target, entry));
  return total;
}

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
      if (/^qemu-system/i.test(p.name)) Object.assign(p, { avd: avdOf(p.command), port: portOf(p.command), cores: Number(p.command?.match(/\s-cores\s+(\d+)/)?.[1] || 1), memoryMB: Number(p.command?.match(/\s-memory\s+(\d+)/)?.[1] || 0),
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
      const text = await this.adb(port, ['shell', "getprop sys.boot_completed; cat /proc/loadavg /proc/uptime; grep -E '^(MemTotal|MemAvailable):' /proc/meminfo; df -k /data | tail -n 1"], { timeout: 5000 }).catch(() => '');
      const lines = text.split(/\r?\n/);
      booted = lines[0]?.trim() === '1';
      const kb = name => Number(text.match(new RegExp(`${name}:\\s+(\\d+)`))?.[1] || 0) * 1024;
      guest = { load: lines[1]?.split(' ').slice(0, 3).map(Number) || [], uptime: Number(lines[2]?.split(' ')[0] || 0), memTotal: kb('MemTotal'), memAvailable: kb('MemAvailable') };
      const df = text.match(/\s(\d+)\s+(\d+)\s+(\d+)\s+\d+%/);
      if (df) guest.storage = { total: Number(df[1]) * 1024, used: Number(df[2]) * 1024 };
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
    // dumpsys and top get their own time limit: a busy or stuck system service must not take the rest with it.
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
      'echo @@gles', "timeout 8 dumpsys SurfaceFlinger 2>/dev/null | grep -m1 '^GLES'",
      'echo @@df', 'df -h /data /sdcard /system 2>/dev/null',
      'echo @@settings', 'settings get secure immersive_mode_confirmations', 'settings get global hide_error_dialogs',
      'echo @@focus', 'timeout 8 dumpsys window 2>/dev/null | grep -m1 mCurrentFocus',
      'echo @@resumed', 'timeout 8 dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity',
      'echo @@guest', `for p in ${packages}; do f=$(pm path $p 2>/dev/null | grep -m1 base.apk | cut -d: -f2); if [ -z $f ]; then echo $p -; else echo $p $(sha256sum $f | cut -d' ' -f1) $(timeout 5 dumpsys package $p 2>/dev/null | grep -m1 versionName | tr -d ' '); fi; done`,
      'echo @@packages', 'timeout 10 pm list packages -3 --show-versioncode',
      // Two samples a second apart: from a single one every process shows 0% CPU.
      'echo @@top', 'timeout 8 top -b -n 2 -d 1 -m 12',
      'echo @@end',
    ].join('; ');
    // Whatever Android answered is kept, even when the shell stopped early (adb exit code, a timeout).
    const result = await this.capture(port, ['shell', script], 60000);
    const text = result.stdout.replace(/\r\n/g, '\n');
    if (!text.includes('@@props')) throw new Error(`Android did not answer the info request (${result.timedOut ? 'timed out' : `adb exit code ${result.code}`}): ${redact(result.stderr.trim().split('\n').pop() || 'no output')}`);
    const reached = [...text.matchAll(/^@@(\w+)$/gm)].map(m => m[1]);
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
      resumed: section(text, 'resumed').match(/u0 (\S+)/)?.[1] || section(text, 'resumed').trim(),
      incomplete: !reached.includes('end') ? { stoppedAfter: reached.at(-1) || '', code: result.code, timedOut: result.timedOut, error: redact(result.stderr.trim().split('\n').slice(-3).join(' ')) } : null,
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

  // adb's stdout and stderr and exit code, whatever the exit code (no exception for a failing command).
  capture(port, args, timeout) {
    return new Promise(resolve => {
      const child = spawn(path.join(this.settings.sdk, 'platform-tools/adb.exe'), ['-s', `emulator-${port}`, ...args], { windowsHide: true });
      let stdout = '', stderr = '', timedOut = false;
      child.stdout.on('data', b => { stdout += b.toString(); });
      child.stderr.on('data', b => { stderr = (stderr + b.toString()).slice(-16384); });
      const timer = setTimeout(() => { timedOut = true; child.kill(); }, timeout);
      child.on('error', e => { clearTimeout(timer); resolve({ code: -1, stdout, stderr: e.message, timedOut }); });
      child.on('close', code => { clearTimeout(timer); resolve({ code, stdout, stderr, timedOut }); });
    });
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

  // A script for Android's root shell. It travels base64-encoded, so no quoting can break on the way through Windows and adb.
  asRoot(port, script, timeout = 60000) {
    return this.capture(port, ['shell', `echo ${Buffer.from(script).toString('base64')} | base64 -d | su 0 sh`], timeout);
  }

  // The Storage tab: what fills Android's /data, and the virtual disk files on this PC.
  async storage() {
    const [port, host] = await Promise.all([this.runtime.findPort(), this.diskInfo()]);
    if (!port) return { collected: new Date().toISOString(), port: null, guest: null, host };
    const result = await this.asRoot(port, storageScript, 120000);
    const text = result.stdout.replace(/\r\n/g, '\n');
    if (!text.includes('@@end')) throw new Error(`Android did not report its storage (${result.timedOut ? 'timed out' : `adb exit code ${result.code}`}): ${redact(result.stderr.trim().split('\n').pop() || 'no output')}`);
    const [, total, used, free] = section(text, 'df').trim().split(/\s+/).map(Number);
    const all = new Set(section(text, 'all').split('\n').map(s => s.trim()).filter(Boolean));
    const components = new Set(guestPackages.map(p => p.package));
    const apps = section(text, 'apps').split('\n').map(line => line.trim().split(' ')).filter(f => f.length === 6).map(([name, ...kb]) => {
      const [apk, data, external, cache, obb] = kb.map(n => Number(n) * 1024);
      return { package: name, apk, data, external, cache, obb, total: apk + data + external + obb, component: components.has(name) };
    }).sort((a, b) => b.total - a.total);
    const leftovers = [
      ...sizeLines(section(text, 'folders')).map(f => ({ ...f, match: f.path.match(leftoverFolder) }))
        .filter(f => f.match && !all.has(f.match[2])).map(({ match, ...f }) => ({ ...f, kind: match[1], package: match[2], name: `Android/${match[1]}/${match[2]}` })),
      ...sizeLines(section(text, 'temp')).filter(f => tempFile.test(f.path)).map(f => ({ ...f, kind: 'temp', name: path.posix.basename(f.path) })),
    ].sort((a, b) => b.size - a.size);
    const sharedFiles = sizeLines(section(text, 'shared')).map(f => ({ ...f, name: path.posix.basename(f.path) })).sort((a, b) => b.size - a.size);
    return { collected: new Date().toISOString(), port, host, guest: { total: total * 1024, used: used * 1024, free: free * 1024, apps, leftovers, shared: sharedFiles } };
  }

  // The AVD's folder on this PC. Its data disk (a qcow2 file) grows as Android stores more and never shrinks by itself.
  avdDirectory() {
    return fs.readFile(path.join(avdHome(), `${this.settings.avd}.ini`), 'utf8').then(t => t.match(/^path=(.+)$/m)?.[1].trim(), () => '')
      .then(dir => dir || path.join(avdHome(), `${this.settings.avd}.avd`));
  }
  async diskInfo() {
    const directory = await this.avdDirectory();
    const config = await fs.readFile(path.join(directory, 'config.ini'), 'utf8').catch(() => null);
    if (config === null) return { exists: false, directory };
    const files = await Promise.all((await fs.readdir(directory)).map(async name => ({ name, size: await folderSize(path.join(directory, name)) })));
    const drive = await fs.statfs(directory).catch(() => null);
    const marker = await fs.readFile(path.join(directory, 'refract-first-boot-pending'), 'utf8').catch(() => null);
    // The disk file's own size is what Android gets; config.ini only matters when the disk is made new.
    const dataSize = await this.diskSize().catch(() => parseSize(config.match(/^disk\.dataPartition\.size\s*=\s*(.+)$/m)?.[1]));
    return { exists: true, directory, dataSize,
      growPending: await exists(path.join(directory, 'refract-grow-pending')),
      total: files.reduce((n, f) => n + f.size, 0), dataDisk: files.filter(f => f.name.startsWith('userdata-qemu.img')).reduce((n, f) => n + f.size, 0),
      snapshots: files.find(f => f.name === 'snapshots')?.size || 0,
      files: files.filter(f => f.size).sort((a, b) => b.size - a.size),
      driveFree: drive ? Number(drive.bavail) * Number(drive.bsize) : 0, driveTotal: drive ? Number(drive.blocks) * Number(drive.bsize) : 0,
      resetPending: marker !== null && marker.includes('wipe-data'), sizes: diskSizes };
  }

  // `pm` answers "Success" or a reason; a failure becomes an error the page shows.
  async pm(port, args) {
    const output = (await this.adb(port, ['shell', 'pm', ...args], { timeout: 120000 }).catch(e => e.message)).trim();
    if (!/^Success/m.test(output)) throw new Error(`Android refused (pm ${args[0]}): ${redact(output.split(/\r?\n/).pop() || 'no answer')}`);
  }
  clearCache(port, pkg) { return this.pm(port, ['clear', '--cache-only', validPackage(pkg)]); }
  clearData(port, pkg) { return this.pm(port, ['clear', validPackage(pkg)]); }
  async uninstall(port, pkg) {
    await this.pm(port, ['uninstall', validPackage(pkg)]);
    // Android keeps an uninstalled app's OBB folder; game files are most of a Quest game's size.
    await this.asRoot(port, `rm -rf ${shared}/Android/obb/${pkg} ${shared}/Android/data/${pkg}`, 60000);
  }
  // Leftover folders of uninstalled apps and files in /data/local/tmp, as the Storage tab listed them.
  async deleteFiles(port, paths) {
    if (!Array.isArray(paths) || !paths.length || paths.length > 500) throw new Error('Select files to delete.');
    const installed = new Set((await this.adb(port, ['shell', 'pm', 'list', 'packages'], { timeout: 30000 })).split(/\r?\n/).map(s => s.replace(/^package:/, '').trim()));
    for (const item of paths) {
      const folder = typeof item === 'string' && item.match(leftoverFolder);
      if (folder ? !/^[A-Za-z0-9_.]+$/.test(folder[2]) || installed.has(folder[2]) : !(typeof item === 'string' && tempFile.test(item))) throw new Error(`Refract does not delete ${item}.`);
    }
    // Several calls when the list is long: each command line has to fit Windows' limit.
    for (let i = 0; i < paths.length; i += 100) {
      const result = await this.asRoot(port, `rm -rf ${paths.slice(i, i + 100).map(shellQuote).join(' ')}; echo @@done`, 300000);
      if (!result.stdout.includes('@@done')) throw new Error(`Android did not finish deleting (${result.timedOut ? 'timed out' : redact(result.stderr.trim() || `adb exit code ${result.code}`)}).`);
    }
  }
  // Saved emulator state (a RAM image as large as Android's memory). Refract always cold-boots (-no-snapshot), so it is
  // never loaded; the caller makes sure no emulator runs this AVD.
  async deleteSnapshots() {
    const directory = path.join(await this.avdDirectory(), 'snapshots');
    for (const entry of await fs.readdir(directory).catch(() => [])) await fs.rm(path.join(directory, entry), { recursive: true, force: true });
  }
  // Make the disk bigger, step 1 (Android running): the one-boot e2fsck wrapper on /system.
  async installGrowWrapper(port) {
    await this.adb(port, ['root'], { timeout: 30000 });
    await new Promise(resolve => setTimeout(resolve, 2000));
    await this.adb(port, ['wait-for-device'], { timeout: 60000 });
    const remount = await this.adb(port, ['remount'], { timeout: 60000 }).catch(e => e.message);
    // An existing e2fsck.real is the real one from an earlier attempt; never replace it with the wrapper.
    const result = await this.asRoot(port, `cd /system/bin || exit 1
[ -f e2fsck.real ] || cp -p e2fsck e2fsck.real || exit 1
chcon u:object_r:fsck_exec:s0 e2fsck.real
echo ${Buffer.from(growWrapper).toString('base64')} | base64 -d > e2fsck.new && chmod 755 e2fsck.new && chown root:shell e2fsck.new && chcon u:object_r:fsck_exec:s0 e2fsck.new && mv e2fsck.new e2fsck || exit 1
sync; head -c 2 e2fsck; echo; echo @@installed`, 60000);
    if (!result.stdout.includes('#!') || !result.stdout.includes('@@installed')) {
      throw new Error(`Android's system files could not be changed (${redact((result.stderr || result.stdout || remount).trim().split('\n').pop() || `adb exit code ${result.code}`)}). Restart Android and try again.`);
    }
  }
  // The data disk's size as Android sees it (the qcow2 file's virtual size; -U reads it while the emulator runs).
  async diskSize() {
    const image = path.join(await this.avdDirectory(), 'userdata-qemu.img.qcow2');
    return JSON.parse(await run(this.qemuImg(), ['info', '-U', '--output=json', image], { timeout: 60000 }))['virtual-size'];
  }
  // Step 2 (emulator stopped): the disk file gets the new size.
  async enlargeDisk(sizeGB) {
    if (!diskSizes.includes(sizeGB)) throw new Error('Choose a disk size.');
    const current = await this.diskSize();
    if (sizeGB * 1024 ** 3 <= current) throw new Error(`Android's disk is already ${Math.round(current / 1024 ** 3)} GB.`);
    const directory = await this.avdDirectory();
    await run(this.qemuImg(), ['resize', path.join(directory, 'userdata-qemu.img.qcow2'), `${sizeGB}G`], { timeout: 120000 });
    await this.setDataSize(directory, sizeGB);
  }
  // Step 3: the next start grows /data into the disk (and in any case puts the real e2fsck back).
  async markGrowPending() { await fs.writeFile(path.join(await this.avdDirectory(), 'refract-grow-pending'), ''); }
  async setDataSize(directory, sizeGB) {
    const file = path.join(directory, 'config.ini');
    const config = await fs.readFile(file, 'utf8').catch(() => { throw new Error(`There is no virtual device named ${this.settings.avd}.`); });
    const line = `disk.dataPartition.size=${sizeGB}G`;
    const next = /^disk\.dataPartition\.size\s*=.*$/m.test(config) ? config.replace(/^disk\.dataPartition\.size\s*=.*$/m, line) : `${config.replace(/\s*$/, '')}\r\n${line}\r\n`;
    if (next !== config) await fs.writeFile(file, next);
  }
  qemuImg() { return path.join(this.settings.sdk, 'emulator/qemu-img.exe'); }
  // Reset Android: the start script boots once with -wipe-data (on the stock emulator, like a new AVD's first boot),
  // which recreates the data disk at the chosen size. Everything installed and every save on Android is gone afterwards.
  async scheduleReset(sizeGB) {
    if (!diskSizes.includes(sizeGB)) throw new Error('Choose a disk size.');
    const directory = await this.avdDirectory();
    await this.setDataSize(directory, sizeGB);
    await fs.writeFile(path.join(directory, 'refract-first-boot-pending'), 'wipe-data');
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
    const port = info.port || await this.runtime.findPort().catch(() => null);
    if (port) {
      const logcat = await this.adb(port, ['logcat', '-d', '-v', 'threadtime'], { timeout: 60000 }).catch(e => `logcat failed: ${e.message}`);
      await fs.writeFile(path.join(directory, 'logcat.txt'), redact(logcat));
      const props = await this.adb(port, ['shell', 'getprop'], { timeout: 10000 }).catch(() => '');
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
