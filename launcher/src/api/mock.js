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
    downloadDir: 'C:\\Users\\you\\Downloads\\Refract' },
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
