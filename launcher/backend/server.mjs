// Refract launcher backend. The Tauri shell starts this with Node and talks to it
// over stdin/stdout, one JSON message per line:
//   shell -> backend  { id, method, args }            request
//                     { type: 'secret', value }       decrypted Meta token at startup
//   backend -> shell  { id, ok, value | error }       response
//                     { event: 'changed', payload }   new public state for the UI
//                     { event: 'launch-error', payload }
//                     { event: 'secret', value }      token to encrypt and store (null = delete)
// Native pieces (windows, file dialogs, credential encryption, opening links) stay
// in the shell; the Meta token never reaches the UI.
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';
import { randomUUID, createHash } from 'node:crypto';
import { MetaAuth, QuestStore, appId } from '../core/meta.mjs';
import { downloadFile, safeName, checkSpace } from '../core/download.mjs';
import { State } from '../core/state.mjs';
import { Runtime, run, validPackage, audioBackends, newUserId, validUserId, validEyeSize, validRenderScale, guestPackages } from '../core/runtime.mjs';
import { Emulator, redact, diskSizes } from '../core/emulator.mjs';
import { checkSetup, fixSetup, headsetStatus, refreshPath } from '../core/setup.mjs';
import { loadLibraryArtwork } from '../core/artwork.mjs';

const directory = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(directory, '../..');
const option = name => { const i = process.argv.indexOf(`--${name}`); return i > 0 ? process.argv[i + 1] : undefined; };
const scan = !process.argv.includes('--no-scan');
const dataDirectory = option('data') || path.join(process.env.APPDATA || path.join(os.homedir(), '.config'), 'Refract');

// stdout is the protocol channel; keep stray logging off it.
const write = process.stdout.write.bind(process.stdout);
console.log = console.info = console.warn = (...args) => process.stderr.write(`${args.join(' ')}\n`);
const send = message => write(`${JSON.stringify(message)}\n`);

let headsetCheck = null;
let state, runtime, emulator, token = '', account = '', accountImage = '', busy = false, fixing = '', fixProgress = '', starting = null, auth = null, emulatorTask = null;
const controllers = new Map();
let artworkTask;
function refreshArtwork() {
  if (artworkTask) return artworkTask;
  artworkTask = loadLibraryArtwork(state.data.games, new QuestStore(), async (id, artwork) => {
    const game = state.data.games.find(g => g.id === id);
    if (game) { Object.assign(game, artwork); await persist(); }
  }).finally(() => { artworkTask = null; });
  return artworkTask;
}
const message = error => redact(error?.message || error);
function publicState() {
  return { ...state.data, signedIn: Boolean(token), account, accountImage, running: runtime.game, runningMode: runtime.mode || null, busy, fixing, fixProgress, starting, emulatorTask,
    // Credentials and signed CDN URLs never reach the UI or library file.
    games: state.data.games.map(g => ({ ...g, files: g.files?.map(f => ({ name: f.name, path: f.path, kind: f.kind, size: f.size })) })) };
}
let changeTimer = null;
function changed() {
  // Coalesce bursts (download progress, artwork) into at most ten UI updates a second.
  if (changeTimer) return;
  changeTimer = setTimeout(() => { changeTimer = null; send({ event: 'changed', payload: publicState() }); }, 100);
}
async function persist() { await state.save(); changed(); }
function getGame(id) { const game = state.data.games.find(g => g.id === id); if (!game) throw new Error('Game is no longer in your library.'); return game; }
function store() { if (!token) throw new Error('Sign in to Meta first.'); return new QuestStore(token); }
async function exclusive(callback) { if (busy) throw new Error('Wait for the current install to finish.'); busy = true; changed(); try { return await callback(); } finally { busy = false; changed(); } }
const absolute = value => { if (typeof value !== 'string' || !path.isAbsolute(value)) throw new Error('Select an absolute path.'); return value; };

async function syncInstalled() {
  const installed = await runtime.installed();
  if (!installed) return false;
  state.data.games = state.data.games.filter(g => !(g.source === 'installed' && g.package?.startsWith('com.refract.')));
  for (const game of state.data.games) game.installed = Boolean(game.package && installed.has(game.package));
  for (const pkg of installed) {
    if (pkg.startsWith('com.refract.') || pkg.startsWith('com.google.') || state.data.games.some(g => g.package === pkg)) continue;
    try {
      const game = await runtime.importInstalled(pkg, path.join(state.directory, 'icons', `${pkg}.png`));
      if (game) state.put(game);
    } catch { /* Non-launchable packages remain outside the games library. */ }
  }
  await persist(); return true;
}
async function syncMeta() {
  const api = store();
  const [result, profile] = await Promise.all([api.library(), api.profile().catch(() => null)]);
  for (const game of state.data.games) if (game.source === 'meta') game.owned = false;
  for (const game of result.games) state.put(game);
  if (profile) { account = profile.name; accountImage = profile.image; }
  await persist();
  refreshArtwork().catch(() => {});
  return { partial: result.partial, count: result.games.length };
}

async function downloadGame(id, binaryId, dlcId) {
  const game = getGame(id);
  if (state.data.jobs.some(j => j.gameId === id && ['queued', 'downloading', 'installing'].includes(j.status))) throw new Error('This game already has an active task.');
  const api = store();
  const job = { id: randomUUID(), gameId: id, name: game.name, status: 'queued', completed: 0, total: 0, binaryId, dlcId, stage: 'Checking Quest build' };
  state.data.jobs.unshift(job); state.data.jobs = state.data.jobs.slice(0, 50);
  const controller = new AbortController(); controllers.set(job.id, controller); await persist();
  (async () => {
    try {
      const plan = dlcId ? { package: game.package, binaryId: game.binaryId, version: game.version, files: [] } : await api.plan(id, binaryId);
      if (dlcId) {
        if (!game.package) throw new Error('Download the base game before its add-ons.');
        const dlc = (await api.dlc(id)).find(d => d.id === dlcId);
        if (!dlc?.owned) throw new Error('Meta did not confirm ownership of this add-on.');
        if (!dlc.files.length) throw new Error('This add-on has no separately downloadable files; it may be included in the base game.');
        plan.files = dlc.files;
      }
      const target = path.join(state.data.settings.downloadDir, appId(id), appId(plan.binaryId));
      for (const file of plan.files) safeName(file.name);
      await checkSpace(target, plan.files.reduce((n, f) => n + Number(f.size || 0), 0));
      job.total = plan.files.reduce((n, f) => n + Number(f.size || 0), 0); job.status = 'downloading';
      await persist();
      const files = [];
      let completed = 0;
      for (const file of plan.files) {
        controller.signal.throwIfAborted();
        job.stage = file.name;
        const destination = path.join(target, safeName(file.name));
        const result = await downloadFile({ url: await api.downloadUrl(file), destination, size: Number(file.size || 0), signal: controller.signal,
          progress: (bytes, total) => { job.completed = completed + bytes; if (!job.total) job.currentTotal = total; changed(); } });
        completed += result.bytes;
        files.push({ name: file.name, path: destination, kind: file.kind, size: result.bytes, sha256: result.sha256 });
      }
      if (!dlcId) {
        const apk = files.find(f => f.kind === 'apk');
        const metadata = await runtime.inspect(apk.path);
        if (metadata.package !== plan.package) throw new Error('Downloaded APK package does not match the selected build.');
        Object.assign(game, { package: metadata.package, activity: metadata.activity, apk: apk.path,
          version: plan.version, binaryId: plan.binaryId, files, downloaded: true });
      } else {
        game.files = [...(game.files || []).filter(f => !files.some(n => n.name === f.name)), ...files];
      }
      state.put(game);
      job.completed = completed; job.total = completed; job.status = 'complete'; job.stage = dlcId ? 'Add-on downloaded' : 'Ready to install';
    } catch (error) {
      job.status = controller.signal.aborted ? 'cancelled' : 'failed'; job.error = message(error);
      if (job.status === 'failed') console.warn(`Download of ${job.name} failed at ${job.stage}: ${job.error}`);
    }
    finally { controllers.delete(job.id); await persist(); }
  })();
  return job.id;
}

const methods = {
  state: () => publicState(),
  // Sign-in: the shell opens Meta's page from authBegin's URL and passes the
  // oculus:// callback it intercepts to authComplete.
  authBegin: async () => { auth = new MetaAuth(); return auth.begin(); },
  authComplete: async callback => {
    if (!auth) throw new Error('Start signing in again.');
    const current = auth; auth = null;
    token = await current.complete(callback);
    send({ event: 'secret', value: token }); changed();
    return syncMeta();
  },
  logout: async () => { for (const controller of controllers.values()) controller.abort(); token = ''; account = ''; accountImage = ''; send({ event: 'secret', value: null }); changed(); },
  sync: async () => { const online = await syncInstalled(); const result = token ? await syncMeta() : null; return { online, meta: result }; },
  builds: id => store().builds(appId(id)).then(items => items.map(b => ({ id: String(b.id), version: b.version, code: b.version_code ?? b.versionCode }))),
  download: (id, binaryId) => downloadGame(appId(id), binaryId),
  dlc: id => store().dlc(appId(id)).then(items => items.map(({ files, ...item }) => ({ ...item, fileCount: files.length, bytes: files.reduce((n, f) => n + f.size, 0) }))),
  downloadDlc: (id, dlcId) => downloadGame(appId(id), null, appId(dlcId)),
  cancel: id => { controllers.get(id)?.abort(); },
  retry: id => { const job = state.data.jobs.find(j => j.id === id); if (!job || !['failed', 'interrupted', 'cancelled'].includes(job.status)) throw new Error('This task cannot be retried.'); return downloadGame(job.gameId, job.binaryId, job.dlcId); },
  // The shell shows the file picker and passes the chosen path(s).
  import: file => exclusive(async () => {
    const game = await runtime.inspect(absolute(file));
    const existing = state.data.games.find(g => g.package === game.package);
    state.put({ ...game, id: existing?.id || game.id, source: existing?.source || game.source, downloaded: true });
    await persist(); return game.package;
  }),
  importAssets: (id, paths) => exclusive(async () => {
    const game = getGame(id);
    if (!Array.isArray(paths)) throw new Error('Select content files.');
    const files = [];
    for (const file of paths) files.push({ path: absolute(file), name: safeName(path.basename(file)), kind: file.endsWith('.obb') ? 'obb' : 'asset', size: (await fs.stat(file)).size });
    game.files = [...(game.files || []).filter(f => !files.some(n => n.name === f.name)), ...files]; await persist();
  }),
  install: id => exclusive(async () => {
    const game = getGame(id);
    // Verify downloaded artifacts before any installation; imported APKs remain user-managed.
    for (const file of game.files || []) if (file.sha256) {
      const hash = createHash('sha256'), handle = await fs.open(file.path);
      try { for await (const chunk of handle.createReadStream()) hash.update(chunk); } finally { await handle.close(); }
      if (hash.digest('hex') !== file.sha256) throw new Error(`${file.name} changed since download. Download it again.`);
    }
    const job = { id: randomUUID(), gameId: id, name: game.name, status: 'installing', stage: 'Preparing install' }; state.data.jobs.unshift(job); await persist();
    try { await runtime.install(game, stage => { job.stage = stage; changed(); }); game.installed = true; job.status = 'complete'; job.stage = 'Installed'; }
    catch (error) { job.status = 'failed'; job.error = message(error); console.warn(`Install of ${job.name} failed at ${job.stage}: ${job.error}`); throw error; }
    finally { await persist(); }
  }),
  // mode 'vr' plays in the headset, 'pc' in a window on this PC (keyboard, mouse or gamepad).
  play: async (id, mode = 'vr') => {
    if (mode !== 'vr' && mode !== 'pc') throw new Error('Invalid play mode.');
    const game = getGame(id); if (busy) throw new Error('Wait for installation to finish.');
    if (!game.installed) throw new Error('Install the game first.');
    if (runtime.child) throw new Error('A game is already running.');
    // Start and prepare Android here (not in the session script) so the Refract runtime is current.
    starting = { gameId: id, stage: mode === 'vr' ? 'Checking VR headset' : 'Starting Android' }; changed();
    try {
      if (mode === 'vr') await runtime.checkHeadset();
      starting = { gameId: id, stage: 'Starting Android' }; changed();
      await exclusive(() => runtime.ensure(stage => { starting = { gameId: id, stage }; changed(); }));
    }
    finally { starting = null; changed(); }
    runtime.launch(game, async (code, tail) => {
      if (code) {
        const error = message(new Error(tail || `Game launcher exited with code ${code}.`));
        state.data.jobs.unshift({ id: randomUUID(), gameId: id, name: game.name, status: 'failed', stage: 'Launch', error });
        send({ event: 'launch-error', payload: `${game.name}: ${error}` });
      }
      await persist();
    }, mode);
    game.lastPlayed = new Date().toISOString(); await persist();
  },
  stop: () => runtime.stop(),
  // Not while a game runs: its session owns the headset, and a probe would compete with it.
  headset: async () => runtime.child ? { connected: runtime.mode === 'vr', runtime: '', busy: true }
    : headsetCheck ??= headsetStatus(runtime).finally(() => { headsetCheck = null; }),
  setup: async () => {
    // An SDK installed since the launcher started (Android Studio's first run) replaces a missing one.
    if (!await isSdk(state.data.settings.sdk)) { const sdk = await findSdk(); if (sdk !== state.data.settings.sdk) { state.data.settings.sdk = sdk; await persist(); } }
    return checkSetup(root, state.data.settings);
  },
  fixSetup: async action => {
    if (fixing) throw new Error('A setup step is already running.');
    if (runtime.child) throw new Error('Close the running game first.');
    fixing = String(action); changed();
    try { await fixSetup(root, state.data.settings, fixing, text => { fixProgress = text; changed(); }); }
    finally { fixing = ''; fixProgress = ''; changed(); }
    return methods.setup();
  },
  // Emulator page. Status is polled while the page is open; logcat is read incrementally by sequence number.
  emulatorStatus: () => emulator.status(),
  emulatorInfo: () => emulator.info(),
  emulatorStorage: async () => {
    const result = await emulator.storage();
    // Library names for the packages the page lists.
    for (const app of result.guest?.apps || []) app.name = state.data.games.find(g => g.package === app.package)?.name || '';
    return { ...result, running: runtime.game?.package || '' };
  },
  emulatorAction: async (action, arg) => {
    if (!Object.hasOwn(emulatorActions, action)) throw new Error('Unknown emulator action.');
    if (emulatorTask) throw new Error('Wait for the current emulator task to finish.');
    const update = stage => { emulatorTask = { action, stage }; changed(); };
    update('');
    try { return await emulatorActions[action](arg, update); } finally { emulatorTask = null; changed(); }
  },
  logcat: (after, generation, follow) => emulator.logcat.read(Number(after) || 0, Number(generation) || 0, follow !== false),
  logcatClear: () => emulator.logcat.clear(),
  logcatExport: async text => {
    if (typeof text !== 'string' || text.length > 64 * 1024 * 1024) throw new Error('Nothing to export.');
    const directory = path.join(state.directory, 'diagnostics'); await fs.mkdir(directory, { recursive: true });
    await fs.writeFile(path.join(directory, `logcat-${new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19)}.txt`), text);
    return { openPath: directory };
  },
  logFiles: () => emulator.logFiles(),
  logFile: id => emulator.logFile(String(id)),
  adbShell: command => emulator.shell(command),
  pidOf: async pkg => {
    validPackage(pkg);
    const port = await runtime.findPort(); if (!port) return [];
    return (await runtime.adbAt(port, ['shell', 'pidof', pkg], { timeout: 5000 }).catch(() => '')).trim().split(/\s+/).filter(Boolean).map(Number);
  },
  exportDiagnostics: async () => {
    const setup = await checkSetup(root, state.data.settings).catch(e => ({ error: message(e) }));
    const result = await emulator.exportDiagnostics({ settings: state.data.settings, running: runtime.game, runningMode: runtime.mode || null, setup,
      jobs: state.data.jobs.slice(0, 10).map(j => ({ name: j.name, status: j.status, stage: j.stage, error: j.error })) });
    return { openPath: result.base, ...result };
  },
  openLogs: async which => {
    const folders = { emulator: path.join(root, 'build-windows-emulator'), game: path.join(root, 'build-windows-game'), data: state.directory,
      screenshots: path.join(state.directory, 'screenshots'), diagnostics: path.join(state.directory, 'diagnostics') };
    const target = folders[which]; if (!target) throw new Error('Unknown folder.');
    await fs.mkdir(target, { recursive: true }); return { openPath: target };
  },
  settings: async values => {
    const allowed = ['sdk', 'avd', 'port', 'memoryMB', 'downloadDir', 'cores', 'showWindow', 'audio', 'hostMic', 'keepEmulator', 'userId',
      'pcEyeSize', 'vrRenderScale'];
    if (!values || typeof values !== 'object') throw new Error('Invalid settings.');
    if (busy || controllers.size || runtime.child) throw new Error('Finish current tasks before changing runtime settings.');
    const settings = { ...state.data.settings };
    for (const key of allowed) if (values[key] !== undefined) settings[key] = values[key];
    if (!/^[A-Za-z0-9_-]+$/.test(settings.avd) || !Number.isInteger(settings.port) || settings.port < 5554 || settings.port > 5682 || settings.port % 2 ||
      !Number.isInteger(settings.memoryMB) || settings.memoryMB < 2048 || settings.memoryMB > 16384) throw new Error('Check the Android AVD, even-numbered port, and memory settings.');
    for (const key of ['sdk', 'downloadDir']) if (typeof settings[key] !== 'string' || !path.isAbsolute(settings[key])) throw new Error('Select absolute Windows paths.');
    if (!Number.isInteger(settings.cores) || settings.cores < 1 || settings.cores > 6 || !audioBackends.includes(settings.audio) ||
      ['showWindow', 'hostMic', 'keepEmulator'].some(key => typeof settings[key] !== 'boolean')) throw new Error('Check the emulator CPU cores (1 to 6) and audio settings.');
    settings.userId = String(settings.userId ?? '').trim();
    if (!validUserId(settings.userId)) throw new Error('The user ID must be a whole number from 1 to 9223372036854775807.');
    if (!validEyeSize(settings.pcEyeSize) || !validRenderScale(settings.vrRenderScale))
      throw new Error('Check the resolution: PC eye size 512 to 4096 (a multiple of 8), VR render scale 25 to 200%.');
    state.data.settings = settings; runtime.settings = settings; await persist();
  },
  // The shell opens these after validating them.
  openFolder: async id => { const game = getGame(id); const target = game.apk ? path.dirname(game.apk) : state.data.settings.downloadDir; await fs.mkdir(target, { recursive: true }); return { openPath: target }; },
  openStore: async id => ({ openUrl: `https://www.meta.com/experiences/${appId(id)}/` }),
};

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function androidPort() { const port = await runtime.findPort(); if (!port) throw new Error('Android is not running.'); return port; }
const noGame = () => { if (runtime.child) throw new Error('Close the running game first.'); };
const adbExe = () => path.join(state.data.settings.sdk, 'platform-tools/adb.exe');
// Debug tools on the Emulator page. Each gets the page's argument and a progress callback.
const emulatorActions = {
  start: (_, update) => exclusive(() => runtime.ensure(update)),
  stop: async (_, update) => {
    noGame();
    const port = await runtime.findPort() ?? await runtime.avdProcessPort();
    if (!port) return;
    update('Stopping Android');
    await runtime.adbAt(port, ['shell', 'sync'], { timeout: 5000 }).catch(() => {});
    await runtime.adbAt(port, ['emu', 'kill'], { timeout: 10000 }).catch(() => {});
    // An emulator that never reached adb ignores `emu kill`; end its process instead.
    for (let i = 0; i < 15 && (await emulator.processes()).some(p => p.avd === state.data.settings.avd); i++) await sleep(1000);
    for (const p of (await emulator.processes()).filter(p => p.avd === state.data.settings.avd)) try { process.kill(p.pid); } catch { /* Already gone. */ }
    runtime.started = false; runtime.found = null;
  },
  reboot: async (_, update) => {
    noGame(); const port = await androidPort();
    update('Restarting Android');
    await runtime.adbAt(port, ['reboot'], { timeout: 30000 }).catch(() => {});
    await sleep(5000); update('Waiting for Android to start'); await runtime.waitBoot(port);
  },
  prepare: (_, update) => exclusive(async () => { noGame(); await runtime.locate(); await androidPort(); await runtime.prepare(update); }),
  reconnect: async () => { await run(adbExe(), ['reconnect', 'offline'], { timeout: 15000 }); },
  restartAdb: async (_, update) => {
    noGame(); emulator.logcat.stop();
    update('Restarting adb'); await run(adbExe(), ['kill-server'], { timeout: 15000 }).catch(() => {});
    await run(adbExe(), ['start-server'], { timeout: 30000 });
  },
  forceStop: async pkg => { await runtime.adbAt(await androidPort(), ['shell', 'am', 'force-stop', validPackage(pkg)], { timeout: 15000 }); },
  screenshot: () => emulator.screenshot(),
  // Storage tab.
  clearCache: async pkg => emulator.clearCache(await androidPort(), notRunning(pkg)),
  clearData: async pkg => emulator.clearData(await androidPort(), notComponent(notRunning(pkg))),
  uninstall: pkg => exclusive(async () => {
    await emulator.uninstall(await androidPort(), notComponent(notRunning(pkg)));
    for (const game of state.data.games) if (game.package === pkg) game.installed = false;
    await persist();
  }),
  deleteFiles: async paths => emulator.deleteFiles(await androidPort(), paths),
  deleteSnapshots: async () => {
    if (await runtime.findPort() || await runtime.avdProcessPort()) throw new Error('Stop the emulator first.');
    await emulator.deleteSnapshots();
  },
  // Grows Android's data disk to sizeGB, keeping everything on it (see Emulator.installGrowWrapper).
  growDisk: (sizeGB, update) => exclusive(async () => {
    noGame();
    if (!diskSizes.includes(sizeGB)) throw new Error('Choose a disk size.');
    if (sizeGB * 1024 ** 3 <= await emulator.diskSize()) throw new Error('Choose a size larger than the disk is now.');
    await runtime.ensure(update);
    update('Preparing Android');
    await emulator.installGrowWrapper(await androidPort());
    // From here on the next start must run the grow boot: it also puts the real e2fsck back.
    let error = null;
    try {
      await emulatorActions.stop(null, update);
      if (await runtime.avdProcessPort()) throw new Error('The emulator did not stop. Close it, then start Android again; the disk keeps its old size.');
      update('Making the disk file bigger');
      await emulator.enlargeDisk(sizeGB);
    } catch (e) { error = e; }
    await emulator.markGrowPending();
    if (error) throw error;
    await runtime.ensure(update);
    const total = Number((await runtime.adb(['shell', 'df', '-k', '/data'])).trim().split(/\r?\n/).pop().split(/\s+/)[1]) * 1024;
    if (!(total > sizeGB * 1024 ** 3 * 0.9)) throw new Error(`The disk file is ${sizeGB} GB now, but Android's storage is still ${(total / 1024 ** 3).toFixed(1)} GB. See the emulator logs.`);
  }),
  // Deletes everything on Android and starts it again from a new data disk of sizeGB.
  resetAndroid: (sizeGB, update) => exclusive(async () => {
    noGame();
    if (!diskSizes.includes(sizeGB)) throw new Error('Choose a disk size.');
    await emulatorActions.stop(null, update);
    if (await runtime.avdProcessPort()) throw new Error('The emulator did not stop. Close it and try again.');
    update('Resetting Android');
    await emulator.scheduleReset(sizeGB);
    await runtime.ensure(update);
    await syncInstalled();
  }),
};
function notRunning(pkg) { if (runtime.game?.package === validPackage(pkg)) throw new Error('Close the game first.'); return pkg; }
function notComponent(pkg) {
  if (guestPackages.some(p => p.package === pkg)) throw new Error('This is part of Refract; Refract would only install it again.');
  return pkg;
}

async function handle({ id, method, args }) {
  try {
    if (!Object.hasOwn(methods, method)) throw new Error(`Unknown launcher request: ${method}`);
    const value = await methods[method](...(Array.isArray(args) ? args : []));
    send({ id, ok: true, value: value ?? null });
  } catch (error) { send({ id, ok: false, error: message(error) }); }
}

const isSdk = async dir => !!dir && fs.access(path.join(dir, 'platform-tools/adb.exe')).then(() => true, () => false);
// Android Studio installs to %LOCALAPPDATA%\Android\Sdk; command-line installs often use ~\Android\Sdk.
async function findSdk() {
  const candidates = [process.env.ANDROID_HOME, process.env.ANDROID_SDK_ROOT,
    path.join(process.env.LOCALAPPDATA || '', 'Android/Sdk'), path.join(os.homedir(), 'Android/Sdk')];
  for (const dir of candidates) if (await isSdk(dir)) return path.resolve(dir);
  return path.join(process.env.LOCALAPPDATA || '', 'Android/Sdk');
}

const loaded = (async () => {
  // Python or the Android SDK may have been installed after the shell started.
  if (scan) await refreshPath();
  state = new State(dataDirectory); await state.load();
  // Digitalis (the ARM64 translator Refract needs) is built for Android 16, so the default AVD is API 36.
  state.data.settings = { avd: 'refract-google-api36', port: 5580, memoryMB: 8192, downloadDir: path.join(os.homedir(), 'Downloads', 'Refract'),
    cores: 6, showWindow: false, audio: 'dsound', hostMic: true, keepEmulator: false, pcEyeSize: 1600, vrRenderScale: 100, ...state.data.settings };
  // A saved SDK path that has no SDK in it came from an earlier default; look for the real one.
  if (!await isSdk(state.data.settings.sdk)) state.data.settings.sdk = await findSdk();
  // Refract no longer needs games patched; drop the old ovrport setting.
  delete state.data.settings.ovrportCli;
  // Every install gets its own Meta user id, or online games see all Refract players as one person. A checkout that
  // already has one for scripts\launch.ps1 (scripts\platform_user_id.txt) keeps it, so its game accounts stay.
  if (!validUserId(state.data.settings.userId)) {
    const kept = (await fs.readFile(path.join(root, 'scripts/platform_user_id.txt'), 'utf8').catch(() => ''))
      .split(/\r?\n/).map(s => s.trim()).find(s => s && !s.startsWith('#'));
    state.data.settings.userId = validUserId(kept) ? kept : newUserId();
    await state.save();
  }
  runtime = new Runtime(root, state.data.settings);
  emulator = new Emulator(runtime, root, state.directory);
})();
const lines = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
lines.on('line', async line => {
  let request; try { request = JSON.parse(line); } catch { return; }
  try { await loaded; } catch (error) { if (request.id !== undefined) send({ id: request.id, ok: false, error: message(error) }); return; }
  if (request.type === 'secret') { token = typeof request.value === 'string' ? request.value : ''; changed(); if (token && scan) syncMeta().catch(() => {}); return; }
  handle(request);
});
// The shell closes stdin when the launcher window closes. A running game keeps going.
lines.on('close', () => {
  for (const controller of controllers.values()) controller.abort();
  emulator?.logcat.stop();
  // The shell kills the backend after five seconds, so stopping Android gets a bounded share of that.
  // Exiting on an empty event loop avoids a libuv assertion process.exit() can hit while child-process
  // handles close on Windows; the timer covers anything still open (a running game's pipes, keep-alive sockets).
  Promise.all([state?.writes, runtime?.shutdown()]).finally(() => setTimeout(() => process.exit(0), 1500).unref());
});
loaded.then(() => {
  if (scan) { syncInstalled().catch(() => {}); refreshArtwork().catch(() => {}); }
}, error => { process.stderr.write(`Refract backend could not start: ${message(error)}\n`); process.exit(1); });
