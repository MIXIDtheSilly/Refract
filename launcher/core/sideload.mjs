// Unknown sources: games that did not come from the Meta store (your own test builds, itch.io releases, ports).
// Kept apart from meta.mjs / the Meta flows so upstream changes to those never conflict with this file.
// Pure helpers only: the backend wiring is backend/sideload.mjs.
import fs from 'node:fs/promises';
import path from 'node:path';

const skipDirs = new Set(['node_modules', '.git', '__macosx']);

// Expansion files are named main.<version>.<package>.obb or patch.<version>.<package>.obb.
export const obbName = pkg => new RegExp(`^(main|patch)\\.\\d+\\.${pkg.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\.obb$`, 'i');

// A cheap "did this file change" mark. Game files are gigabytes, so no hashing.
export const stamp = stat => `${stat.size}:${Math.floor(stat.mtimeMs)}`;
export const stampOf = file => fs.stat(file).then(stamp, () => '');

// .apk and .obb files under a folder, a few levels deep (itch.io downloads and build folders nest them).
export async function findFiles(folder, { depth = 3, limit = 500 } = {}) {
  const found = [];
  const walk = async (dir, level) => {
    let entries;
    try { entries = await fs.readdir(dir, { withFileTypes: true }); } catch { return; }
    for (const entry of entries) {
      if (found.length >= limit) return;
      const full = path.join(dir, entry.name), ext = path.extname(entry.name).toLowerCase();
      if (entry.isDirectory()) { if (level < depth && !skipDirs.has(entry.name.toLowerCase())) await walk(full, level + 1); }
      else if (entry.isFile() && (ext === '.apk' || ext === '.obb')) {
        const stat = await fs.stat(full).catch(() => null);
        if (stat) found.push({ path: full, name: entry.name, kind: ext === '.apk' ? 'apk' : 'obb', size: stat.size, mtimeMs: stat.mtimeMs });
      }
    }
  };
  await walk(path.resolve(folder), 0);
  return found;
}

// Expansion files that belong to a package: next to its APK, or in an obb/<package> folder beside it.
export async function siblingObbs(apk, pkg) {
  const dir = path.dirname(apk), match = obbName(pkg), found = new Map();
  for (const where of [dir, path.join(dir, pkg), path.join(dir, 'obb', pkg), path.join(dir, 'Android', 'obb', pkg)])
    for (const file of await findFiles(where, { depth: 0 })) if (file.kind === 'obb' && match.test(file.name)) found.set(file.name, file);
  return [...found.values()].map(({ path: p, name, size }) => ({ path: p, name, kind: 'obb', size }));
}

// APKs found in a folder, newest per package, each with its expansion files. `inspect` reads an APK's identity.
// Anything that cannot be added is reported in `problems` instead of failing the whole scan.
export async function planFolder(folder, inspect) {
  const files = await findFiles(folder);
  const apks = files.filter(f => f.kind === 'apk').sort((a, b) => b.mtimeMs - a.mtimeMs);
  const games = new Map(), problems = [];
  for (const apk of apks) {
    let info;
    try { info = await inspect(apk.path); } catch (error) { problems.push(`${apk.name}: ${error.message}`); continue; }
    if (games.has(info.package)) continue;  // An older build of the same game.
    games.set(info.package, { info, obbs: files.filter(f => f.kind === 'obb' && obbName(info.package).test(f.name)) });
  }
  const used = new Set([...games.values()].flatMap(g => g.obbs.map(o => o.path)));
  for (const obb of files.filter(f => f.kind === 'obb' && !used.has(f.path))) problems.push(`${obb.name}: no APK in the folder has this package, skipped`);
  if (!apks.length) problems.push('No APK files found in that folder.');
  return { games: [...games.values()], problems };
}

// The library entry for an inspected APK. `existing` keeps the id, Android state and the player's choices.
export function entryFor(info, obbs, existing, apkStat) {
  const files = obbs.map(({ path: p, name, size }) => ({ path: p, name, kind: 'obb', size }));
  return {
    ...existing,
    id: existing?.id || info.id, package: info.package, activity: info.activity, name: existing?.unknownSource ? existing.name : info.name,
    version: info.version, vrSdk: info.vrSdk, image: info.image || existing?.image || '', apk: info.apk,
    // A reinstall pushes these; content files the player added by hand to an existing entry stay.
    files: [...(existing?.files || []).filter(f => !files.some(n => n.name === f.name)), ...files],
    source: 'local', unknownSource: true, downloaded: true,
    apkStamp: apkStat ? stamp(apkStat) : existing?.apkStamp || '',
    owned: existing?.unknownSource ? Boolean(existing.owned) : false,
    addedAt: existing?.addedAt || new Date().toISOString(),
  };
}

// What changed on disk since the entry was added or last installed.
export async function diskStatus(game) {
  if (!game.apk) return { missing: true, changed: false, obbMissing: [] };
  const stat = await fs.stat(game.apk).catch(() => null);
  const obbMissing = [];
  for (const file of game.files || []) if (file.kind === 'obb' && !await fs.access(file.path).then(() => true, () => false)) obbMissing.push(file.name);
  if (!stat) return { missing: true, changed: false, obbMissing };
  const seen = game.installedStamp || game.apkStamp;
  return { missing: false, changed: Boolean(seen) && seen !== stamp(stat), obbMissing };
}
