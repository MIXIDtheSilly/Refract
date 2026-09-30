// Sample backend for previewing the UI in a browser. Nothing here touches Meta,
// Android or the file system.
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const hours = n => new Date(Date.now() - n * 3600000).toISOString();
const listeners = new Set();
const errorListeners = new Set();

const catalog = [
  { id: '2448060205267927', name: 'Yeeps: Hide and Seek', publisher: 'Trass Games', genres: ['Action', 'Casual'], price: 'Free' },
  { id: '7255396864545733', name: 'Pinball FX', publisher: 'Zen Studios', genres: ['Arcade', 'Simulation'], price: '$0.00' },
  { id: '5719805344724799', name: 'Orbit Drift', publisher: 'Sample Studio', genres: ['Racing'], price: '$14.99' },
  { id: '4061278657325523', name: 'Glass Garden', publisher: 'Sample Studio', genres: ['Puzzle', 'Relaxation'], price: '$9.99' },
  { id: '6147843588585221', name: 'Night Signal', publisher: 'Low Tide', genres: ['Adventure'], price: '$19.99' },
  { id: '3719855918132311', name: 'Paper Planets', publisher: 'Fold Games', genres: ['Casual', 'Exploration'], price: 'Free' },
  { id: '5102030405060708', name: 'Refraction Lab', publisher: 'Prism Works', genres: ['Puzzle'], price: '$7.99' },
  { id: '4455667788990011', name: 'Summit Climb', publisher: 'Highline', genres: ['Sports', 'Fitness'], price: '$24.99' },
];
const describe = name => `${name} is sample data used to preview the Refract launcher. In the real launcher this text comes from the Meta Quest store listing.`;

const state = {
  signedIn: true, account: 'Quest player', running: null, busy: false,
  settings: { sdk: 'C:\\Users\\you\\AppData\\Local\\Android\\Sdk', avd: 'refract-google-api36', port: 5580, memoryMB: 8192,
    downloadDir: 'C:\\Users\\you\\Downloads\\Refract', cores: 6, showWindow: false, audio: 'dsound', hostMic: true, keepEmulator: false },
  emulatorTask: null,
  games: [
    { ...catalog[0], source: 'meta', owned: true, installed: true, downloaded: true, apk: 'yeeps.apk', package: 'com.TrassGames.G2Companion', version: '1.42.0', lastPlayed: hours(3), description: describe('Yeeps') },
    { ...catalog[1], source: 'meta', owned: true, installed: true, downloaded: true, apk: 'pinball.apk', package: 'com.zenstudios.PFX', version: '2.1.4', lastPlayed: hours(30), description: describe('Pinball FX') },
    { ...catalog[2], source: 'meta', owned: true, downloaded: true, apk: 'orbit.apk', package: 'com.sample.orbitdrift', version: '0.9.1', description: describe('Orbit Drift') },
    { ...catalog[3], source: 'meta', owned: true, description: describe('Glass Garden') },
    { ...catalog[4], source: 'meta', owned: true, description: describe('Night Signal') },
    { id: 'local:com.example.handtracking', name: 'Hand Tracking Sample', source: 'local', installed: true, downloaded: true, apk: 'sample.apk', package: 'com.example.handtracking', activity: 'com.example.handtracking/.Main', version: '1.0' },
  ],
  jobs: [
    { id: 'job-1', gameId: catalog[1].id, name: 'Pinball FX', status: 'complete', stage: 'Installed' },
  ],
};

// One missing item so the preview shows the setup banner and a fix button.
const setupChecks = [
  { id: 'python', title: 'Python', ok: true, detail: 'Python 3.13.15' },
  { id: 'sdk', title: 'Android SDK', ok: true, detail: 'C:\\Users\\you\\AppData\\Local\\Android\\Sdk' },
  { id: 'avd', title: 'Virtual device', ok: false, detail: 'There is no virtual device named refract-google-api36.',
    fix: { action: 'android', label: 'Set up Android', note: 'Downloads about 2 GB and accepts the Android SDK licenses.' } },
  { id: 'hypervisor', title: 'Hardware acceleration', ok: true, detail: 'WHPX(10.0.26200) is installed and usable.' },
  { id: 'components', title: 'Refract components', ok: true, detail: 'Host bridge, GPU sharing layer, Android runtime and platform stand-in are built.' },
  { id: 'openxr', title: 'PC VR runtime', ok: true, detail: 'Meta Horizon Link is the active OpenXR runtime.' },
];

// Emulator page sample data: a running emulator and a logcat that keeps growing.
const emu = { on: true, seq: 0, lines: [], generation: 1, booted: Date.now() - 3600e3 };
const sampleLines = [['I', 'Unity', 'Loaded scene MainMenu in 412 ms'], ['D', 'RefractXR', 'xrWaitFrame: predicted 11.1 ms, pose seq 18231'],
  ['W', 'OVRPlatform', 'Entitlement check answered by the Refract stand-in'], ['I', 'ActivityManager', 'Displayed com.TrassGames.G2Companion/.MainActivity for user 0: +2s311ms'],
  ['E', 'AudioTrack', 'getTimestamp: status -19'], ['V', 'libEGL', 'eglMakeCurrent: 0x3000'], ['I', 'native_bridge', 'Translated 1,204 new regions (Digitalis)'],
  ['D', 'OpenXR-Loader', 'Found runtime com.oculus.systemdriver']];
function logcatLines(n) {
  const pad = (v, w = 2) => String(v).padStart(w, '0');
  for (let i = 0; i < n; i++) {
    const [level, tag, message] = sampleLines[Math.floor(Math.random() * sampleLines.length)];
    const d = new Date();
    emu.lines.push([++emu.seq, `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`,
      tag === 'Unity' || tag === 'RefractXR' ? 4312 : 1180, 4312 + (i % 7), level, tag, message]);
  }
  if (emu.lines.length > 60000) emu.lines.splice(0, emu.lines.length - 60000);
}
logcatLines(400);
const guestProps = { 'ro.product.manufacturer': 'Oculus', 'ro.product.model': 'Quest 3', 'ro.build.version.release': '16', 'ro.build.version.sdk': '36',
  'ro.build.fingerprint': 'google/sdk_gphone64_x86_64/emu64xa:16/BP22.250325.006/13344233:userdebug/dev-keys', 'ro.product.cpu.abilist': 'x86_64,arm64-v8a',
  'ro.dalvik.vm.native.bridge': 'libberberis_arm64.so', 'ro.hardware.vulkan': 'ranchu', 'debug.refract.gpu_share': '1', 'sys.boot_completed': '1' };
const sdkPath = 'C:\\Users\\you\\AppData\\Local\\Android\\Sdk';

const snapshot = () => structuredClone(state);
const emit = () => { const value = snapshot(); for (const listener of listeners) listener(value); };
const find = id => { const game = state.games.find(g => g.id === id); if (!game) throw new Error('Game is no longer in your library.'); return game; };

async function simulateDownload(job, game) {
  job.status = 'downloading'; job.total = 2.6 * 1024 ** 3; job.completed = 0; job.stage = `${game.package || 'game'}.apk`; emit();
  while (job.completed < job.total) {
    await wait(120);
    if (job.status === 'cancelled') return;
    job.completed = Math.min(job.total, job.completed + job.total / 60); emit();
  }
  Object.assign(game, { downloaded: true, apk: `${game.id}.apk`, package: game.package || `com.sample.${game.name.toLowerCase().replace(/[^a-z]/g, '')}`, version: game.version || '1.0.0' });
  job.status = 'complete'; job.stage = 'Ready to install'; emit();
}

const methods = {
  state: () => snapshot(),
  sync: async () => { await wait(900); return { online: true, meta: state.signedIn ? { partial: false, count: state.games.length } : null }; },
  login: async () => { await wait(1200); state.signedIn = true; state.account = 'Quest player'; emit(); return { partial: false, count: state.games.length }; },
  logout: async () => { state.signedIn = false; state.account = ''; emit(); },
  builds: async () => { await wait(400); return [{ id: '901', version: '1.42.0', code: 142 }, { id: '900', version: '1.41.3', code: 141 }, { id: '870', version: '1.38.0', code: 138 }]; },
  download: async id => {
    const game = find(id);
    if (state.jobs.some(j => j.gameId === id && ['queued', 'downloading', 'installing'].includes(j.status))) throw new Error('This game already has an active task.');
    const job = { id: `job-${Date.now()}`, gameId: id, name: game.name, status: 'queued', stage: 'Checking Quest build', completed: 0, total: 0 };
    state.jobs.unshift(job); emit(); simulateDownload(job, game); return job.id;
  },
  dlc: async () => { await wait(400); return [{ id: '11', name: 'Soundtrack pack', owned: true, fileCount: 1, bytes: 180 * 1024 ** 2 }, { id: '12', name: 'Season pass', owned: false, fileCount: 2, bytes: 1.2 * 1024 ** 3 }, { id: '13', name: 'Cosmetic bundle', owned: true, fileCount: 0, bytes: 0 }]; },
  downloadDlc: async id => methods.download(id),
  cancel: async id => { const job = state.jobs.find(j => j.id === id); if (job) { job.status = 'cancelled'; job.error = 'Cancelled.'; } emit(); },
  retry: async id => { const job = state.jobs.find(j => j.id === id); state.jobs = state.jobs.filter(j => j !== job); return methods.download(job.gameId); },
  import: async () => {
    await wait(600);
    const game = { id: 'local:com.example.imported', name: 'Imported Game', source: 'local', downloaded: true, apk: 'imported.apk', package: 'com.example.imported', version: '0.1' };
    if (!state.games.some(g => g.id === game.id)) state.games.push(game); emit(); return game.package;
  },
  importAssets: async () => { await wait(300); },
  install: async id => {
    const game = find(id); state.busy = true;
    const job = { id: `job-${Date.now()}`, gameId: id, name: game.name, status: 'installing', stage: 'Starting Android' };
    state.jobs.unshift(job); emit();
    for (const stage of ['Starting Android', 'Installing APK', 'Copying expansion files']) { job.stage = stage; emit(); await wait(700); }
    game.installed = true; job.status = 'complete'; job.stage = 'Installed'; state.busy = false; emit();
  },
  play: async (id, mode = 'vr') => {
    const game = find(id); if (!game.installed) throw new Error('Install the game first.');
    if (mode === 'vr') throw new Error('No VR headset is connected. Connect your headset (Meta Horizon Link or Air Link, or SteamVR) and press Play again.');
    await wait(400); state.running = id; state.runningMode = mode; game.lastPlayed = new Date().toISOString(); emit();
  },
  stop: async () => { await wait(500); state.running = null; state.runningMode = null; emit(); },
  // The preview has no headset, so it shows the notice and puts Play on PC first.
  headset: async () => { await wait(300); return state.running ? { connected: false, busy: true } : { connected: false, runtime: 'Meta Horizon Link', title: 'No VR headset connected', detail: 'Put on your Quest and connect it with Quest Link (USB cable) or Air Link.' }; },
  setup: async () => { await wait(600); return structuredClone(setupChecks); },
  fixSetup: async action => {
    state.fixing = action; emit(); await wait(1500);
    for (const check of setupChecks) if (check.fix?.action === action) { check.ok = true; delete check.fix; }
    state.fixing = ''; emit(); return structuredClone(setupChecks);
  },
  settings: async values => {
    if (!/^[A-Za-z0-9_-]+$/.test(values.avd) || values.port % 2 || values.port < 5554 || values.port > 5682) throw new Error('Check the Android AVD, even-numbered port, and memory settings.');
    state.settings = { ...state.settings, ...values }; emit();
  },
  chooseFolder: async () => 'C:\\Games\\Refract',
  emulatorStatus: async () => {
    await wait(200);
    const qemu = emu.on ? { pid: 21344, name: 'qemu-system-x86_64-multicore.exe', path: `${sdkPath}\\emulator\\qemu\\windows-x86_64\\qemu-system-x86_64-multicore.exe`,
      avd: state.settings.avd, port: 5580, cores: state.settings.cores, multicore: true, window: state.settings.showWindow, cpu: 1200, cpuPercent: 18 + Math.random() * 8,
      memory: 5.1 * 1024 ** 3, threads: 142, started: new Date(emu.booted).toISOString() } : null;
    return { state: emu.on ? 'running' : 'stopped', port: emu.on ? 5580 : null, serial: emu.on ? 'emulator-5580' : '', configuredPort: 5580, avd: state.settings.avd,
      adopted: false, startedHere: true, game: state.running, qemu,
      guest: emu.on ? { load: [3.1, 2.8, 2.4], uptime: (Date.now() - emu.booted) / 1000, memTotal: 8 * 1024 ** 3, memAvailable: 3.2 * 1024 ** 3 } : null,
      processes: qemu ? [{ ...qemu, command: `${qemu.path} -avd ${qemu.avd} -port 5580 -gpu host -accel on -no-snapshot -no-window -cores ${qemu.cores}` },
        { pid: 9012, name: 'adb.exe', cpuPercent: 0.2, memory: 18e6 }] : [],
      logcat: { running: emu.on, lines: emu.lines.length } };
  },
  emulatorInfo: async () => {
    await wait(700);
    const now = new Date().toISOString();
    return { collected: now, port: emu.on ? 5580 : null,
      host: { os: 'Microsoft Windows 11 Home 10.0.26200', cpu: 'Sample CPU', cores: 12, threads: 20, memory: 32 * 1024 ** 3, free: 14 * 1024 ** 3,
        gpus: ['NVIDIA GeForce RTX 3080 (driver 32.0.16.1656)'], openxr: 'C:\\Program Files\\Meta Horizon\\Support\\oculus-runtime\\oculus_openxr_64.json',
        acceleration: { ok: true, detail: 'WHPX(10.0.26200) is installed and usable.' }, sdk: state.settings.sdk, emulatorVersion: '37.1.11', multicoreQemu: true,
        adbVersion: 'Android Debug Bridge version 1.0.41', node: 'v24.19.0', launcherVersion: '0.2.0', commit: '48f1dae sample', root: 'C:\\Refract',
        data: 'C:\\Users\\you\\AppData\\Roaming\\Refract' },
      guest: emu.on ? { props: guestProps, clocksource: 'tsc', cores: 6, loadavg: '3.10 2.80 2.40', uptime: 3600,
        memory: { total: 8 * 1024 ** 3, available: 3.2 * 1024 ** 3, swapTotal: 0, swapFree: 0 },
        kernel: 'Linux localhost 6.6.66-android15 #1 SMP PREEMPT x86_64', cmdline: 'tsc=nowatchdog idle=poll', selinux: 'Permissive',
        display: 'Physical size: 1080x1920 · Physical density: 420', gles: 'NVIDIA Corporation, NVIDIA GeForce RTX 3080/PCIe/SSE2, OpenGL ES 3.2',
        storage: 'Filesystem        Size Used Avail Use% Mounted on\n/dev/block/dm-5    12G 4.1G  7.9G  35% /data', immersiveConfirmed: true,
        guestPackages: [{ package: 'com.refract.openxrruntime', installed: true, sha256: 'a', version: '0.1' }, { package: 'com.oculus.systemdriver', installed: true, sha256: 'b', version: '0.1' },
          { package: 'com.oculus.horizon', installed: false, sha256: '', version: '' }],
        packages: state.games.filter(g => g.installed && g.package).map(g => ({ package: g.package, versionCode: '142' })),
        top: '  PID USER         PR  NI VIRT  RES  SHR S[%CPU] %MEM     TIME+ ARGS\n 4312 u0_a142      10 -10  14G 2.1G 180M S  310  26.8  12:31.04 com.TrassGames.G2Companion\n  512 system       -2  -8  12G  88M  60M S  4.0   1.1   0:40.12 surfaceflinger' } : null,
      components: [{ label: 'Host bridge', built: true, modified: now }, { label: 'Refract OpenXR runtime', package: 'com.refract.openxrruntime', built: true, modified: now, sha256: 'a' },
        { label: 'Refract XR driver', package: 'com.oculus.systemdriver', built: true, modified: now, sha256: 'x' },
        { label: 'Meta Platform stand-in', package: 'com.oculus.horizon', built: true, modified: now, sha256: 'c' }] };
  },
  emulatorAction: async action => {
    state.emulatorTask = { action, stage: action === 'start' ? 'Starting Android' : '' }; emit(); await wait(900);
    if (action === 'start') { emu.on = true; emu.booted = Date.now(); }
    if (action === 'stop') emu.on = false;
    state.emulatorTask = null; emit();
    if (action === 'screenshot') return { path: 'C:\\Users\\you\\AppData\\Roaming\\Refract\\screenshots\\android.png',
      image: 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==' };
    return null;
  },
  logcat: async (after, generation) => {
    if (emu.on) logcatLines(Math.floor(Math.random() * 12));
    const reset = generation !== emu.generation;
    return { generation: emu.generation, reset, entries: emu.lines.filter(e => e[0] > (reset ? 0 : after)), more: false,
      running: emu.on, serial: emu.on ? 'emulator-5580' : '', error: '', capacity: 60000 };
  },
  logcatClear: async () => { emu.lines = []; emu.generation++; },
  logcatExport: async () => null,
  logFiles: async () => [{ id: 'emulator-err', label: 'Emulator (stderr)', exists: true, size: 3615, modified: new Date().toISOString() },
    { id: 'launcher', label: 'Launcher backend', exists: false, size: 0, modified: '' }],
  logFile: async id => (id === 'emulator-err'
    ? { id, label: 'Emulator (stderr)', file: 'C:\\Refract\\build-windows-emulator\\emulator.stderr.log', exists: true, size: 3615, modified: new Date().toISOString(),
      text: 'Refract GPU layer: instance active\nRefract GPU: matching adapter NVIDIA GeForce RTX 3080, D3D11=0x0\nRefract GPU layer: device active, sharing=1\n' }
    : { id, label: 'Launcher backend', exists: false, text: '' }),
  adbShell: async command => { await wait(250); return { code: 0, output: `sample output of: ${command}`, ms: 250, serial: 'emulator-5580' }; },
  pidOf: async pkg => (state.games.some(g => g.package === pkg && g.id === state.running) ? [4312] : []),
  exportDiagnostics: async () => { await wait(800); return null; },
  openLogs: async () => null,
  openFolder: async () => null,
  openStore: async () => null,
};

export async function call(method, ...args) {
  if (!methods[method]) throw new Error(`Unknown launcher request: ${method}`);
  await wait(60);
  return methods[method](...args);
}
export const onChange = callback => { listeners.add(callback); return () => listeners.delete(callback); };
export const onLaunchError = callback => { errorListeners.add(callback); return () => errorListeners.delete(callback); };
export const windowControls = null;
