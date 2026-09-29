// Opt-in live validation on Windows: node launcher/verify-meta.mjs [--app=<id>]
// Uses the launcher's DPAPI-encrypted session; never prints tokens, signed file
// URLs, or account identifiers.
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { QuestStore } from './core/meta.mjs';
import { run } from './core/runtime.mjs';
const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const session = path.join(process.env.APPDATA || '', 'Refract', 'meta-session.dpapi');

async function token() {
  const script = `Add-Type -AssemblyName System.Security; $b = [IO.File]::ReadAllBytes('${session.replaceAll("'", "''")}'); ` +
    `[Console]::Out.Write([Text.Encoding]::UTF8.GetString([Security.Cryptography.ProtectedData]::Unprotect($b, $null, 'CurrentUser')))`;
  return run('powershell.exe', ['-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')]);
}

try {
  const api = new QuestStore(await token()), report = {};
  try {
    const library = await api.library();
    report.library = { count: library.games.length, partial: library.partial, games: library.games.map(g => ({ id: g.id, name: g.name, platform: g.platform })) };
    const id = process.argv.find(a => /^--app=\d+$/.test(a))?.split('=')[1];
    if (id) {
      try { const builds = await api.builds(id); report.builds = builds.slice(0, 5).map(b => ({ id: b.id, version: b.version, platform: b.platform, code: b.version_code })); }
      catch (e) { report.buildError = e.message; }
      try { const plan = await api.plan(id); report.plan = { ...plan, files: plan.files.map(({ uri, ...file }) => file) }; }
      catch (e) { report.planError = e.message; }
      try { const dlc = await api.dlc(id); report.dlc = dlc.map(d => ({ id: d.id, name: d.name, owned: d.owned, files: d.files.map(({ uri, ...f }) => f) })); }
      catch (e) { report.dlcError = e.message; }
    }
  } catch (error) { report.error = error.message; }
  await fs.mkdir(path.join(root, 'build-launcher-validation'), { recursive: true });
  await fs.writeFile(path.join(root, 'build-launcher-validation/meta-report.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report));
} catch {
  console.error('Live validation could not open the encrypted Meta session. Sign in from the launcher first.');
  process.exit(1);
}
