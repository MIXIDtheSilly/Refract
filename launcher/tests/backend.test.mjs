import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const server = fileURLToPath(new URL('../backend/server.mjs', import.meta.url));

// Talks to the backend the way the Tauri shell does: JSON lines over stdio.
function start(t, data) {
  const child = spawn(process.execPath, [server, '--data', data, '--no-scan'], { stdio: ['pipe', 'pipe', 'inherit'] });
  const waiting = new Map(), events = [];
  readline.createInterface({ input: child.stdout }).on('line', line => {
    const message = JSON.parse(line);
    if (message.id !== undefined) waiting.get(message.id)?.(message); else events.push(message);
  });
  let next = 1;
  const request = (method, ...args) => new Promise(resolve => { const id = next++; waiting.set(id, resolve); child.stdin.write(`${JSON.stringify({ id, method, args })}\n`); });
  const exited = new Promise(resolve => child.on('exit', resolve));
  t.after(() => child.kill());
  return { child, request, events, exited };
}

test('backend answers over stdio, validates settings and keeps secrets out of state', { timeout: 15000 }, async t => {
  const data = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-backend-'));
  t.after(() => fs.rm(data, { recursive: true, force: true }));
  const { child, request, events, exited } = start(t, data);

  const state = await request('state');
  assert.equal(state.ok, true);
  assert.equal(state.value.signedIn, false);
  assert.equal(state.value.settings.avd, 'refract-google-api36');

  assert.match((await request('settings', { port: 5581 })).error, /even-numbered port/);
  assert.match((await request('nope')).error, /Unknown launcher request/);
  assert.match((await request('toString')).error, /Unknown launcher request/);
  assert.match((await request('import', 'relative.apk')).error, /absolute path/);
  assert.match((await request('builds', '123456')).error, /Sign in to Meta first/);

  child.stdin.write(`${JSON.stringify({ type: 'secret', value: 'FRLtest-token' })}\n`);
  const signedIn = await request('state');
  assert.equal(signedIn.value.signedIn, true);
  assert.doesNotMatch(JSON.stringify(signedIn.value), /FRLtest-token/);

  const logout = await request('logout');
  assert.equal(logout.ok, true);
  assert.ok(events.some(e => e.event === 'secret' && e.value === null));

  const saved = await request('settings', { port: 5582, sdk: data, downloadDir: data });
  assert.equal(saved.ok, true);
  child.stdin.end();
  assert.equal(await exited, 0);
  assert.equal(JSON.parse(await fs.readFile(path.join(data, 'library.json'), 'utf8')).settings.port, 5582);
});
