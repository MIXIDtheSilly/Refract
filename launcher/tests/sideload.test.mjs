import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { diskStatus, findFiles, obbName, planFolder, siblingObbs } from '../core/sideload.mjs';
import { sideloadMethods } from '../backend/sideload.mjs';
import { State } from '../core/state.mjs';

const pkg = 'com.example.game';
async function folder(t, files) {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-sideload-'));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  for (const [name, text] of Object.entries(files)) { await fs.mkdir(path.dirname(path.join(dir, name)), { recursive: true }); await fs.writeFile(path.join(dir, name), text); }
  return dir;
}
// Stands in for Runtime.inspect: the file's text is the package.
const inspect = async apk => {
  const text = (await fs.readFile(apk, 'utf8')).split('\n')[0].trim();
  if (text === 'bad') throw new Error('not an APK');
  return { package: text, activity: `${text}/.Main`, name: `Game ${text}`, version: '1.0', image: '', vrSdk: 'openxr', apk: path.resolve(apk), source: 'local', id: `local:${text}` };
};

test('expansion file names must carry the game package', () => {
  assert.ok(obbName(pkg).test(`main.12.${pkg}.obb`));
  assert.ok(obbName(pkg).test(`patch.3.${pkg}.OBB`));
  assert.ok(!obbName(pkg).test(`main.12.${pkg}x.obb`));
  assert.ok(!obbName(pkg).test(`main.12.comXexampleXgame.obb`));
});

test('a folder scan finds nested APKs, keeps the newest per package and matches expansion files', async t => {
  const dir = await folder(t, { 'a/old.apk': pkg, 'a/main.1.com.example.game.obb': 'x', 'b/other.apk': 'com.example.other', 'b/readme.txt': 'hi', 'c/stray.obb': 'x', 'c/broken.apk': 'bad' });
  await fs.utimes(path.join(dir, 'a/old.apk'), new Date(2020, 0, 1), new Date(2020, 0, 1));
  await fs.writeFile(path.join(dir, 'b/new.apk'), pkg);
  const { games, problems } = await planFolder(dir, inspect);
  assert.deepEqual(games.map(g => g.info.package).sort(), [pkg, 'com.example.other']);
  const main = games.find(g => g.info.package === pkg);
  assert.equal(path.basename(main.info.apk), 'new.apk');
  assert.deepEqual(main.obbs.map(o => o.name), [`main.1.${pkg}.obb`]);
  assert.ok(problems.some(p => /broken\.apk: not an APK/.test(p)));
  assert.ok(problems.some(p => /stray\.obb/.test(p)));
});

test('findFiles stops at the depth limit and ignores node_modules', async t => {
  const dir = await folder(t, { 'one/two/three/four/deep.apk': 'x', 'node_modules/m.apk': 'x', 'ok.apk': 'x' });
  assert.deepEqual((await findFiles(dir)).map(f => f.name), ['ok.apk']);
});

test('siblingObbs reads the APK folder and an obb/<package> folder beside it', async t => {
  const dir = await folder(t, { 'g.apk': pkg, [`main.1.${pkg}.obb`]: 'x', [`obb/${pkg}/patch.2.${pkg}.obb`]: 'x', 'main.1.other.obb': 'x' });
  assert.deepEqual((await siblingObbs(path.join(dir, 'g.apk'), pkg)).map(f => f.name).sort(), [`main.1.${pkg}.obb`, `patch.2.${pkg}.obb`]);
});

function backend(directory, installs = []) {
  const state = new State(directory);
  const ctx = {
    state, runtime: { inspect, game: null }, persist: () => state.save(), absolute: v => { if (!path.isAbsolute(v)) throw new Error('Select an absolute path.'); return v; },
    getGame: id => { const g = state.data.games.find(x => x.id === id); if (!g) throw new Error('Game is no longer in your library.'); return g; },
    exclusive: cb => cb(), methods: { install: async id => { installs.push(id); } },
  };
  return { state, methods: sideloadMethods(ctx) };
}

test('adding an APK makes a separate unknown-source entry that is not owned by default', async t => {
  const dir = await folder(t, { 'g.apk': pkg, [`main.1.${pkg}.obb`]: 'x' });
  const { state, methods } = backend(dir);
  const result = await methods.sideloadAdd(path.join(dir, 'g.apk'));
  assert.deepEqual(result, { added: [`Game ${pkg}`], problems: [] });
  const [game] = state.data.games;
  assert.equal(game.unknownSource, true);
  assert.equal(game.source, 'local');
  assert.equal(game.owned, false);
  assert.equal(game.files.length, 1);
  await methods.sideloadOwned(game.id, true);
  assert.equal(game.owned, true);
  // Adding it again keeps the player's choice.
  await methods.sideloadAdd(path.join(dir, 'g.apk'));
  assert.equal(state.data.games.length, 1);
  assert.equal(state.data.games[0].owned, true);
  assert.match((await methods.sideloadAdd('relative.apk')).problems[0], /absolute/);
});

test('a game from the Meta library is never taken over', async t => {
  const dir = await folder(t, { 'g.apk': pkg });
  const { state, methods } = backend(dir);
  state.data.games.push({ id: '123', name: 'Meta Game', package: pkg, source: 'meta', owned: true });
  const result = await methods.sideloadAdd(path.join(dir, 'g.apk'));
  assert.deepEqual(result.added, []);
  assert.match(result.problems[0], /Meta library/);
  assert.equal(state.data.games[0].source, 'meta');
  await assert.rejects(methods.sideloadOwned('123', true), /not from an unknown source/);
});

test('a rebuilt APK shows as changed until it is installed again', async t => {
  const dir = await folder(t, { 'g.apk': pkg });
  const installs = [];
  const { state, methods } = backend(dir, installs);
  await methods.sideloadAdd(path.join(dir, 'g.apk'));
  const game = state.data.games[0];
  assert.deepEqual(await diskStatus(game), { missing: false, changed: false, obbMissing: [] });
  await fs.writeFile(path.join(dir, 'g.apk'), `${pkg}\n// rebuilt`);
  assert.equal((await methods.sideloadStatus())[game.id].changed, true);
  await methods.sideloadInstall(game.id);
  assert.deepEqual(installs, [game.id]);
  assert.equal((await methods.sideloadStatus())[game.id].changed, false);
  await fs.rm(path.join(dir, 'g.apk'));
  assert.equal((await methods.sideloadStatus())[game.id].missing, true);
});

test('an APK that turns into another game cannot replace the entry, and removing keeps files', async t => {
  const dir = await folder(t, { 'g.apk': pkg });
  const { state, methods } = backend(dir);
  await methods.sideloadAdd(path.join(dir, 'g.apk'));
  const id = state.data.games[0].id;
  await fs.writeFile(path.join(dir, 'g.apk'), 'com.example.different');
  await assert.rejects(methods.sideloadInstall(id), /now com\.example\.different/);
  await methods.sideloadRemove(id);
  assert.equal(state.data.games.length, 0);
  assert.ok(await fs.access(path.join(dir, 'g.apk')).then(() => true, () => false));
});
