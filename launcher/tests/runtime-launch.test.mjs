import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { Runtime, headsetProblem, readableError, run, powershellArgs, newUserId, validUserId } from '../core/runtime.mjs';

test('each install gets its own Meta user id', () => {
  const ids = new Set(Array.from({ length: 1000 }, newUserId));
  assert.equal(ids.size, 1000);
  for (const id of ids) { assert.ok(validUserId(id)); assert.ok(BigInt(id) >= 10n ** 15n && BigInt(id) < 2n ** 53n); }
  assert.ok(validUserId('28315021954821081') && validUserId('1') && validUserId('9223372036854775807'));
  for (const bad of ['', '0', '012', '-5', '1.5', '12a', '9223372036854775808', 123]) assert.equal(validUserId(bad), false, String(bad));
});

test('PowerShell CLIXML errors become the one readable message', () => {
  // What Windows PowerShell wrote when the emulator quit (seen in the launcher before this fix).
  const clixml = '#< CLIXML\r\n<Objs Version="1.1.0.1" xmlns="http://schemas.microsoft.com/powershell/2004/04"><Obj S="progress" RefId="0"><TN RefId="0"><T>System.Management.Automation.PSCustomObject</T><T>System.Object</T></TN><MS><I64 N="SourceId">1</I64><PR N="Record"><AV>Preparing modules for first use.</AV><AI>0</AI><Nil /><PI>-1</PI><PC>-1</PC><T>Completed</T><SR>-1</SR><SD> </SD></PR></MS></Obj>'
    + '<S S="Error">Emulator exited; see C:\\Refract\\build-windows-emulator_x000D__x000A_</S><S S="Error">At C:\\Refract\\tools\\windows_android_emulator.ps1:108 char:35_x000D__x000A_</S>'
    + '<S S="Error">+ ...        if ($Process.HasExited) { throw "Emulator exited; see $logs" }_x000D__x000A_</S><S S="Error">+                                      ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~_x000D__x000A_</S>'
    + '<S S="Error">    + CategoryInfo          : OperationStopped: (Emulator exited...indows-emulator:String) [], RuntimeException_x000D__x000A_</S><S S="Error">    + FullyQualifiedErrorId : Emulator exited; see C:\\Refract\\build-windows-emulator_x000D__x000A_</S><S S="Error"> _x000D__x000A_</S></Objs>';
  assert.equal(readableError(clixml), 'Emulator exited; see C:\\Refract\\build-windows-emulator');
  assert.equal(readableError('Starting\nREFRACT-ERROR: The virtual device is already open.\n'), 'The virtual device is already open.');
  // The end of a session's Write-Host log, cut mid-record (what the launcher showed a player once).
  const fragment = 'bj N="Tags" RefId="5"><TNRef RefId="2" /><LST><S>PSHOST</S></LST></Obj></Props></Obj><Obj S="information" RefId="12"><TNRef RefId="0" /><ToString>Refract game session stopped.</ToString></Obj></Objs>';
  assert.equal(readableError(fragment), '');
});

test('OpenXR errors from the host bridge become what the player should do', () => {
  assert.match(headsetProblem('Refract OpenXR: xrGetSystem failed: XR_ERROR_FORM_FACTOR_UNAVAILABLE (-35)'), /^No VR headset is connected/);
  assert.match(headsetProblem('Refract OpenXR: xrCreateInstance failed: XR_ERROR_RUNTIME_UNAVAILABLE (-51)'), /^The PC VR runtime is not running/);
  assert.equal(headsetProblem('something else'), '');
});

test('Play on PC runs the session script with -PcViewer, VR without it', { skip: process.platform !== 'win32', timeout: 30000 }, async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-launch-'));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  await fs.mkdir(path.join(root, 'tools'));
  await fs.writeFile(path.join(root, 'tools/run_windows_game.ps1'), `
param($Avd, $Port, $Sdk, $MemoryMB, $Package, $Activity, $GameName, [switch]$Owned, [switch]$PcViewer, $UserId)
Write-Output "PC:$PcViewer USER:$UserId"
exit 3
`);
  const runtime = new Runtime(root, { avd:'test',port:5580,sdk:root,memoryMB:8192,userId:'28315021954821081' });
  const game = { id:'local:com.example.game',package:'com.example.game',activity:'com.example.game/.Main',name:'Test' };
  for (const [mode, expected] of [['pc', 'PC:True USER:28315021954821081'], ['vr', 'PC:False USER:28315021954821081']]) {
    const output = await new Promise(resolve => { runtime.launch(game, (code, text) => resolve(text), mode); assert.equal(runtime.mode, mode); });
    assert.match(output, new RegExp(expected));
    assert.equal(runtime.mode, null);
  }
});

test('a game session that fails reports its error even after a long log', { skip: process.platform !== 'win32', timeout: 30000 }, async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-launch-'));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  await fs.mkdir(path.join(root, 'tools'));
  await fs.writeFile(path.join(root, 'tools/run_windows_game.ps1'), `
param($Avd, $Port, $Sdk, $MemoryMB, $Package, $Activity, $GameName)
try { 1..40 | ForEach-Object { Write-Host "$GameName | Refract is running. Closing its window stops this game session." }; throw 'No VR headset is connected.' }
finally { Write-Host 'Refract game session stopped.' }
`);
  const runtime = new Runtime(root, { avd:'test',port:5580,sdk:root,memoryMB:8192 });
  const result = await new Promise(resolve => {
    runtime.launch({ id:'local:com.example.game',package:'com.example.game',activity:'com.example.game/.Main',name:'Test' }, (code, output) => resolve({code,output}));
    t.after(() => runtime.child?.kill());
  });
  assert.equal(result.code, 1);
  assert.equal(result.output, 'No VR headset is connected.');
});

test('a script that throws reports only its message', { skip: process.platform !== 'win32', timeout: 30000 }, async t => {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-ps-'));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  const script = path.join(dir, 'fails.ps1');
  await fs.writeFile(script, "param($Name)\n$ErrorActionPreference = 'Stop'\nWrite-Host 'working'\nthrow \"$Name could not start.\"\n");
  await assert.rejects(run('powershell.exe', powershellArgs(script, { Name: "Game's emulator" })), { message: "Game's emulator could not start." });
});

test('Windows game launch actually executes PowerShell and reports its exit', { skip: process.platform !== 'win32', timeout: 15000 }, async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-launch-'));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  await fs.mkdir(path.join(root, 'tools'));
  await fs.writeFile(path.join(root, 'tools/run_windows_game.ps1'), `
param($Avd, $Port, $Sdk, $MemoryMB, $Package, $Activity, $GameName)
Write-Output "EXECUTED:$Package"
exit 7
`);
  const runtime = new Runtime(root, { avd:'test',port:5580,sdk:root,memoryMB:8192 });
  const result = await new Promise(resolve => {
    runtime.launch({ id:'local:com.example.game',package:'com.example.game',activity:'com.example.game/.Main',name:'Test' }, (code, output) => resolve({code,output}));
    t.after(() => runtime.child?.kill());
  });
  assert.equal(result.code, 1); // PowerShell wrapper normalizes failed script exits.
  assert.match(result.output, /EXECUTED:com.example.game/);
  assert.equal(runtime.game, null);
});

test('an emulator already running the configured AVD on another port is used instead of starting one', async () => {
  // Fake adb: which ports answer, and which AVD each one runs.
  const fake = (running) => {
    const runtime = new Runtime('.', { avd: 'refract-google-api36', port: 5580, sdk: '.', memoryMB: 8192 });
    runtime.adbAt = async (port, args) => {
      if (!port) return `List of devices attached\r\n${Object.keys(running).map(p => `emulator-${p}\tdevice\r\n`).join('')}\r\n`;
      if (!running[port]) throw new Error('device offline');
      if (args[0] === 'get-state') return 'device\r\n';
      if (args[0] === 'emu') return `${running[port]}\r\nOK\r\n`;
      if (args.join(' ') === 'shell getprop sys.boot_completed') return '1\r\n';
      return '';
    };
    runtime.avdProcessPort = async () => null;
    runtime.prepare = async () => { runtime.prepared = runtime.port; };
    return runtime;
  };
  let runtime = fake({ 5580: 'refract-google-api36' });
  assert.equal(await runtime.locate(), 5580); assert.equal(runtime.port, 5580);

  runtime = fake({ 5554: 'other-avd', 5582: 'refract-google-api36' });
  assert.equal(await runtime.locate(), 5582); assert.equal(runtime.port, 5582);
  await runtime.ensure();  // Resolves without starting a second copy of the AVD.
  assert.equal(runtime.prepared, 5582);
  assert.equal(await runtime.online(), true);

  // Still booting: the process holds the AVD but adb does not list it as a device yet.
  runtime = fake({});
  runtime.avdProcessPort = async () => 5584;
  runtime.adbAt = async (port, args) => port === 5584 && args.join(' ') === 'shell getprop sys.boot_completed' ? '1\r\n' : Promise.reject(new Error('offline'));
  await runtime.ensure();
  assert.equal(runtime.prepared, 5584);

  runtime = fake({ 5554: 'other-avd' });
  assert.equal(await runtime.locate(), null); assert.equal(runtime.port, 5580);
  runtime = fake({});
  assert.equal(await runtime.online(), false);
});
