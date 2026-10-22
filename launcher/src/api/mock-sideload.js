// Sample unknown-sources backend for the browser preview (see backend/sideload.mjs for the real one).
export function sideloadMock({ state, emit, find, wait }) {
  const sample = { id: 'local:com.example.testbuild', name: 'Test Build (itch.io)', package: 'com.example.testbuild', version: '0.3.1', source: 'local',
    unknownSource: true, downloaded: true, apk: 'testbuild.apk', owned: false, files: [{ name: 'main.3.com.example.testbuild.obb', kind: 'obb', size: 900 * 1024 ** 2 }] };
  const add = async () => {
    await wait(500);
    if (!state.games.some(g => g.id === sample.id)) state.games.push({ ...sample });
    emit(); return { added: [sample.name], problems: [] };
  };
  return {
    sideloadAdd: add,
    sideloadScan: add,
    sideloadStatus: async () => Object.fromEntries(state.games.filter(g => g.unknownSource).map(g => [g.id, { missing: false, changed: Boolean(g.installed), obbMissing: [] }])),
    sideloadInstall: async id => { const game = find(id); await wait(900); game.installed = true; emit(); },
    sideloadOwned: async (id, owned) => { find(id).owned = owned; emit(); },
    sideloadRemove: async id => { state.games = state.games.filter(g => g.id !== id); emit(); },
  };
}
