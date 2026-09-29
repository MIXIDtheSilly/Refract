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
import { Runtime } from '../core/runtime.mjs';
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

let state, runtime, token = '', account = '', busy = false, auth = null;
const controllers = new Map();
const searchResults = new Map();
let artworkTask;
function refreshArtwork() {
  if (artworkTask) return artworkTask;
  artworkTask = loadLibraryArtwork(state.data.games, new QuestStore(), async (id, artwork) => {
    const game = state.data.games.find(g => g.id === id);
    if (game) { Object.assign(game, artwork); await persist(); }
  }).finally(() => { artworkTask = null; });
  return artworkTask;
}
const message = error => String(error?.message || error).replace(/(?:OC|FRL|EA)[A-Za-z0-9_|-]{30,}/g, '[redacted]').replace(/access_token=[^\s&]+/g, 'access_token=[redacted]');
function publicState() {
  return { ...state.data, signedIn: Boolean(token), account, running: runtime.game, busy,
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
  const result = await store().library();
  for (const game of state.data.games) if (game.source === 'meta') game.owned = false;
  for (const game of result.games) state.put(game);
  account = result.name; await persist();
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
    } catch (error) { job.status = controller.signal.aborted ? 'cancelled' : 'failed'; job.error = message(error); }
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
  logout: async () => { for (const controller of controllers.values()) controller.abort(); token = ''; account = ''; send({ event: 'secret', value: null }); changed(); },
  sync: async () => { const online = await syncInstalled(); const result = token ? await syncMeta() : null; return { online, meta: result }; },
  search: async text => { const games = await new QuestStore(token).search(String(text)); for (const game of games) searchResults.set(game.id, game); return games; },
  add: async id => { const game = searchResults.get(appId(id)); if (!game) throw new Error('Search for this app again.'); const existing = state.data.games.find(g => g.id === game.id); if (!existing) state.put(game); await persist(); return game.id; },
  lookup: async input => { const game = await store().details(appId(input)); state.put(game); await persist(); return game.id; },
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
    catch (error) { job.status = 'failed'; job.error = message(error); throw error; }
    finally { await persist(); }
  }),
  play: async id => {
    const game = getGame(id); if (busy) throw new Error('Wait for installation to finish.');
    if (!game.installed) throw new Error('Install the game first.');
    runtime.launch(game, async (code, tail) => {
      if (code) {
        const error = message(new Error(tail || `Game launcher exited with code ${code}.`));
        state.data.jobs.unshift({ id: randomUUID(), gameId: id, name: game.name, status: 'failed', stage: 'Launch', error });
        send({ event: 'launch-error', payload: `${game.name}: ${error}` });
      }
      await persist();
    });
    game.lastPlayed = new Date().toISOString(); await persist();
  },
  stop: () => runtime.stop(),
  settings: async values => {
    const allowed = ['sdk', 'avd', 'port', 'memoryMB', 'downloadDir'];
    if (!values || typeof values !== 'object') throw new Error('Invalid settings.');
    if (busy || controllers.size || runtime.child) throw new Error('Finish current tasks before changing runtime settings.');
    const settings = { ...state.data.settings };
    for (const key of allowed) if (values[key] !== undefined) settings[key] = values[key];
    if (!/^[A-Za-z0-9_-]+$/.test(settings.avd) || !Number.isInteger(settings.port) || settings.port < 5554 || settings.port > 5682 || settings.port % 2 ||
      !Number.isInteger(settings.memoryMB) || settings.memoryMB < 2048 || settings.memoryMB > 16384) throw new Error('Check the Android AVD, even-numbered port, and memory settings.');
    for (const key of ['sdk', 'downloadDir']) if (typeof settings[key] !== 'string' || !path.isAbsolute(settings[key])) throw new Error('Select absolute Windows paths.');
    state.data.settings = settings; runtime.settings = settings; await persist();
  },
  // The shell opens these after validating them.
  openFolder: async id => { const game = getGame(id); const target = game.apk ? path.dirname(game.apk) : state.data.settings.downloadDir; await fs.mkdir(target, { recursive: true }); return { openPath: target }; },
  openStore: async id => ({ openUrl: id ? `https://www.meta.com/experiences/${appId(id)}/` : 'https://www.meta.com/experiences/' }),
};

async function handle({ id, method, args }) {
  try {
    if (!Object.hasOwn(methods, method)) throw new Error(`Unknown launcher request: ${method}`);
    const value = await methods[method](...(Array.isArray(args) ? args : []));
    send({ id, ok: true, value: value ?? null });
  } catch (error) { send({ id, ok: false, error: message(error) }); }
}

const loaded = (async () => {
  state = new State(dataDirectory); await state.load();
  // Digitalis (the ARM64 translator Refract needs) is built for Android 16, so the default AVD is API 36.
  state.data.settings = { sdk: path.join(process.env.LOCALAPPDATA || '', 'Android/Sdk'), avd: 'refract-google-api36', port: 5580,
    memoryMB: 8192, downloadDir: path.join(os.homedir(), 'Downloads', 'Refract'), ...state.data.settings };
  // Refract no longer needs games patched; drop the old ovrport setting.
  delete state.data.settings.ovrportCli;
  runtime = new Runtime(root, state.data.settings);
})();
const lines = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
lines.on('line', async line => {
  let request; try { request = JSON.parse(line); } catch { return; }
  try { await loaded; } catch (error) { if (request.id !== undefined) send({ id: request.id, ok: false, error: message(error) }); return; }
  if (request.type === 'secret') { token = typeof request.value === 'string' ? request.value : ''; changed(); if (token && scan) syncMeta().catch(() => {}); return; }
  handle(request);
});
// The shell closes stdin when the launcher window closes. A running game keeps going.
lines.on('close', () => { for (const controller of controllers.values()) controller.abort(); state?.writes.finally(() => process.exit(0)); });
loaded.then(() => {
  if (scan) { syncInstalled().catch(() => {}); refreshArtwork().catch(() => {}); }
}, error => { process.stderr.write(`Refract backend could not start: ${message(error)}\n`); process.exit(1); });
