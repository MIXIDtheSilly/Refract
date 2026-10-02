import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Camera, ChevronsDown, CircleCheck, Copy, CornerDownLeft, Download, Eraser, FileArchive, FolderOpen, HardDrive, Loader2, PackageCheck, PackageX, Pause, Play, Plug, Power, RefreshCw, RotateCcw, ScrollText, Server, Square, Trash2, TriangleAlert, X } from 'lucide-react';
import { call } from '../api';
import { ago, bytes } from '../components/common';
import { Chip, Highlight, LogList, SearchBox, useMatcher } from '../components/log-view';
import { useSettingsForm } from '../components/settings-form';

const tabs = [['overview', 'Overview'], ['storage', 'Storage'], ['logcat', 'Logcat'], ['logs', 'Log files'], ['shell', 'Shell'], ['settings', 'Settings']];
const levels = ['V', 'D', 'I', 'W', 'E', 'F'];
const levelNames = { V: 'Verbose', D: 'Debug', I: 'Info', W: 'Warning', E: 'Error', F: 'Fatal' };
// Per-viewer conveniences only; the page works the same without storage.
const remembered = (key, fallback) => { try { return localStorage.getItem(`refract.emulator.${key}`) || fallback; } catch { return fallback; } };
const remember = (key, value) => { try { localStorage.setItem(`refract.emulator.${key}`, value); } catch { /* Storage unavailable. */ } };
const errorText = error => String(error?.message || error);
const number = n => Number(n || 0).toLocaleString();
function duration(seconds) {
  seconds = Math.floor(seconds || 0);
  const d = Math.floor(seconds / 86400), h = Math.floor(seconds / 3600) % 24, m = Math.floor(seconds / 60) % 60;
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : m ? `${m}m ${seconds % 60}s` : `${seconds}s`;
}
const formatEntry = e => e[1] ? `${e[1]} ${String(e[2]).padStart(5)} ${String(e[3]).padStart(5)} ${e[4]} ${e[5]}: ${e[6]}` : e[6];
async function copy(text, notify) {
  try { await navigator.clipboard.writeText(text); notify('Copied'); } catch { notify('Could not copy to the clipboard.', true); }
}

// Emulator status, polled every few seconds while the page is open (one request at a time).
// The last few minutes of CPU and memory readings feed the Overview's small graphs.
function useStatus() {
  const [status, setStatus] = useState(null);
  const [error, setError] = useState('');
  const refresh = useCallback(async () => {
    try {
      const next = await call('emulatorStatus');
      const sample = { cpu: next.qemu?.cpuPercent ?? null, memory: next.guest?.memTotal ? 1 - next.guest.memAvailable / next.guest.memTotal : null };
      setStatus(old => ({ ...next, history: [...(old?.history || []), sample].slice(-45) }));
      setError('');
    } catch (e) { setError(errorText(e)); }
  }, []);
  useEffect(() => {
    let stopped = false, timer;
    const loop = async () => { await refresh(); if (!stopped) timer = setTimeout(loop, 4000); };
    loop();
    return () => { stopped = true; clearTimeout(timer); };
  }, [refresh]);
  return [status, refresh, error];
}

// The page's copy of the backend's logcat buffer, fetched incrementally while the Logcat tab is open.
// It survives switching tabs; leaving the page drops it (the backend still has its own copy).
function useLogcat(active) {
  const buffer = useRef([]);
  const cursor = useRef({ seq: 0, generation: 0 });
  const [version, setVersion] = useState(0);
  const [meta, setMeta] = useState({ running: false, serial: '', error: '', capacity: 60000 });
  const [paused, setPaused] = useState(false);
  useEffect(() => {
    if (!active || paused) return undefined;
    let stopped = false, timer;
    const poll = async () => {
      try {
        const result = await call('logcat', cursor.current.seq, cursor.current.generation);
        if (stopped) return;
        if (result.reset) { buffer.current = []; cursor.current.seq = 0; }
        if (result.entries.length) {
          let next = buffer.current.concat(result.entries);
          if (next.length > result.capacity) next = next.slice(next.length - result.capacity);
          buffer.current = next; cursor.current.seq = next[next.length - 1][0];
        }
        if (result.entries.length || result.reset) setVersion(v => v + 1);
        cursor.current.generation = result.generation;
        setMeta({ running: result.running, serial: result.serial, error: result.error, capacity: result.capacity });
        timer = setTimeout(poll, result.more ? 30 : 700);
      } catch (e) {
        if (stopped) return;
        setMeta(m => ({ ...m, running: false, error: errorText(e) }));
        timer = setTimeout(poll, 3000);
      }
    };
    poll();
    return () => { stopped = true; clearTimeout(timer); };
  }, [active, paused]);
  const clear = async () => {
    await call('logcatClear');
    buffer.current = []; cursor.current = { seq: 0, generation: 0 }; setVersion(v => v + 1);
  };
  return { entries: buffer.current, version, meta, paused, setPaused, clear };
}

// PIDs of an Android package, refreshed while it is the logcat filter (they change when the app restarts).
function usePids(pkg) {
  const [pids, setPids] = useState([]);
  useEffect(() => {
    setPids([]);
    if (!pkg) return undefined;
    let stopped = false, timer;
    const poll = async () => {
      const value = await call('pidOf', pkg).catch(() => []);
      if (stopped) return;
      setPids(old => (old.join() === value.join() ? old : value));
      timer = setTimeout(poll, 3000);
    };
    poll();
    return () => { stopped = true; clearTimeout(timer); };
  }, [pkg]);
  return pids;
}

const stateLabels = { running: 'Running', booting: 'Android is starting', starting: 'Emulator is starting', stopped: 'Stopped' };

export function EmulatorPage({ state, run, pending, notify }) {
  const [tab, setTabState] = useState(() => remembered('tab', 'overview'));
  const setTab = value => { setTabState(value); remember('tab', value); };
  const [status, refreshStatus, statusError] = useStatus();
  const [info, setInfo] = useState(null);
  const loadInfo = useCallback(() => run('emulator-info', async () => setInfo(await call('emulatorInfo'))), [run]);
  const online = status?.state === 'running';
  // Once the first status is in, and again whenever Android comes up or moves to another port.
  const known = Boolean(status);
  useEffect(() => { if (known) loadInfo(); }, [loadInfo, known, online, status?.port]);
  const logcat = useLogcat(tab === 'logcat');
  const [app, setApp] = useState('');
  const game = state.running && state.games.find(g => g.id === state.running);

  const task = state.emulatorTask || (state.starting && { stage: state.starting.stage });
  const action = (name, arg, done) => run(`emu-${name}`, async () => {
    const result = await call('emulatorAction', name, arg);
    await refreshStatus();
    done?.(result);
    return result;
  });
  const busy = Boolean(state.emulatorTask) || Boolean(state.starting);
  const up = status && status.state !== 'stopped';

  return <div className="page emulator-page">
    <header className="page-head">
      <div>
        <h1 className="page-title">Emulator</h1>
        <div className="page-sub">{status ? <>{status.avd} · {status.serial || `port ${status.configuredPort}`}{status.adopted && ` (adopted; Refract is set to ${status.configuredPort})`}</> : statusError || 'Checking the emulator…'}</div>
      </div>
      <div className="emu-actions">
        <span className={`headset-chip emu-chip ${status?.state || ''}`}><span className="dot" />{status ? stateLabels[status.state] : 'Checking'}</span>
        {online && <button type="button" className="btn btn-outline btn-sm" disabled={busy || Boolean(state.running)} title={state.running ? 'Close the running game first' : undefined}
          onClick={() => action('reboot', null, () => notify('Android restarted'))}><RotateCcw />Restart Android</button>}
        {up
          ? <button type="button" className="btn btn-outline btn-sm" disabled={busy || Boolean(state.running)} title={state.running ? 'Close the running game first' : undefined}
            onClick={() => action('stop', null, () => notify('Emulator stopped'))}><Power />Stop</button>
          : <button type="button" className="btn btn-primary btn-sm" disabled={busy || !status} onClick={() => action('start', null, () => notify('Android is ready'))}><Play />Start</button>}
      </div>
    </header>
    {task && <div className="task-line" role="status"><Loader2 className="spin" />{task.stage || 'Working…'}</div>}
    <div className="segmented emu-tabs" role="tablist">
      {tabs.map(([id, label]) => <button key={id} type="button" role="tab" aria-selected={tab === id} aria-pressed={tab === id} onClick={() => setTab(id)}>{label}
        {id === 'logcat' && logcat.meta.running && <span className="dot live" />}</button>)}
    </div>

    {tab === 'overview' && <Overview {...{ state, status, info, loadInfo, pending, run, notify, action, busy, game }}
      onLogcat={pkg => { setApp(pkg); setTab('logcat'); }} onStorage={() => setTab('storage')} />}
    {tab === 'storage' && <Storage {...{ state, status, run, pending, notify, action, busy }} />}
    {tab === 'logcat' && <Logcat {...{ logcat, info, game, app, setApp, run, notify, status }} />}
    {tab === 'logs' && <LogFiles run={run} pending={pending} notify={notify} />}
    {tab === 'shell' && <Shell online={online} />}
    {tab === 'settings' && <EmulatorSettings key={JSON.stringify(state.settings)} {...{ state, run, pending, notify, up }} />}
  </div>;
}

function Tile({ label, value, sub, tone, chart, meter }) {
  return <div className={`tile ${tone || ''}`}><small>{label}</small><strong>{value ?? '—'}</strong>{sub && <span>{sub}</span>}
    {chart && <Sparkline values={chart} />}
    {meter != null && <div className="meter"><div style={{ width: `${Math.min(100, meter * 100)}%` }} /></div>}</div>;
}
// Values from 0 to 1, oldest first; gaps (null) break the line.
function Sparkline({ values }) {
  if (values.filter(v => v != null).length < 2) return <div className="spark" />;
  const step = 100 / 44, start = 100 - (values.length - 1) * step;
  const points = values.map((v, i) => (v == null ? null : `${(start + i * step).toFixed(2)},${(30 - Math.max(0, Math.min(1, v)) * 28).toFixed(2)}`));
  const runs = points.reduce((all, p) => { if (p) all[all.length - 1].push(p); else if (all.at(-1).length) all.push([]); return all; }, [[]]).filter(r => r.length > 1);
  return <svg className="spark" viewBox="0 0 100 31" preserveAspectRatio="none" aria-hidden="true">
    {runs.map(r => <polyline key={r[0]} points={r.join(' ')} vectorEffect="non-scaling-stroke" />)}</svg>;
}
// The last `top` sample (the first of two counts no CPU time yet), busiest first, without Refract's own probe.
function parseTop(text) {
  const lines = String(text || '').split('\n'), header = lines.findLastIndex(l => /^\s*PID\s+USER/.test(l));
  const size = s => { const m = String(s).match(/^([\d.]+)([KMGT]?)$/); return m ? Number(m[1]) * 1024 ** ' KMGT'.indexOf(m[2] || ' ') : 0; };
  return lines.slice(header + 1).map(l => l.trim().split(/\s+/)).filter(f => f.length >= 11)
    .map(f => ({ pid: f[0], user: f[1], res: size(f[5]), cpu: Number(f[8]) || 0, memory: Number(f[9]) || 0, name: f.slice(11).join(' ') }))
    .filter(p => !/^(top -b|timeout \d+ top|sh -c echo @@props)/.test(p.name))
    .sort((a, b) => b.cpu - a.cpu || b.res - a.res).slice(0, 8);
}
function KeyValues({ rows }) {
  return <dl className="kv">{rows.filter(r => r && r[1] !== undefined && r[1] !== null && r[1] !== '').map(([key, value, className]) =>
    <div key={key}><dt>{key}</dt><dd className={className || ''}>{value}</dd></div>)}</dl>;
}
// A problem value; the reason and what to do about it show on hover.
const bad = (value, reason) => <span className="bad" title={reason}>{value}</span>;

// What can be wrong with this emulator, from the status and the snapshot: [label, problem or '', fix].
function healthChecks({ state, status, info }) {
  const guest = info?.guest, host = info?.host, qemu = status?.qemu, live = status?.guest, props = guest?.props || {};
  const setup = ['prepare', 'Fix', 'Sets up Android for Refract again (Android may restart)'];
  const restart = ['restart', 'Restart emulator', 'Stops the emulator and starts it with the current settings'];
  const checks = [];
  if (guest) {
    const outdated = info.components.filter(c => c.package).filter(c => {
      const g = guest.guestPackages.find(p => p.package === c.package);
      return !g?.installed || g.sha256 !== c.sha256;
    });
    checks.push(
      ['Quest identity', /oculus/i.test(props['ro.product.manufacturer']) ? '' : `Android presents itself as ${props['ro.product.manufacturer']} ${props['ro.product.model']}; games won’t use the Meta Platform stand-in`, setup],
      ['ARM translator', props['ro.dalvik.vm.native.bridge'] === 'libberberis_arm64.so' ? '' : 'Digitalis is not Android’s native bridge; ARM64 games won’t start', setup],
      ['Refract components', outdated.length ? `${outdated.map(c => c.label).join(', ')} ${outdated.length === 1 ? 'is' : 'are'} missing or older than this build` : '', setup],
      ['GPU', /swiftshader|llvmpipe|software/i.test(guest.gles || '') ? `Software rendering (${guest.gles})` : ''],
      ['Clock', guest.clocksource === 'tsc' ? '' : `Clock source is ${guest.clocksource || 'unknown'}, not the TSC; games run slower`, ['reboot', 'Restart Android', 'Each boot gets the TSC or not by chance']],
      ['Focus', guest.immersiveConfirmed && guest.errorDialogsHidden ? '' : 'Android may show a notice or crash dialog over games', setup],
      ['Dialogs', /Application Error|Not Responding|isn.t responding|keeps stopping/i.test(guest.focus) ? `A dialog has focus: ${guest.focus}` : ''],
    );
  }
  if (qemu) {
    // Only memory: the start script lowers the vCPU count on PCs with few performance cores.
    const memory = qemu.memoryMB && qemu.memoryMB !== state.settings.memoryMB;
    checks.push(
      ['vCPUs', !qemu.multicore && state.settings.cores > 1 ? 'Stock qemu: Android runs on one vCPU' : '', restart],
      ['Settings', memory ? `Running with ${number(qemu.memoryMB)} MB of memory; Settings say ${number(state.settings.memoryMB)} MB` : '', restart],
    );
  }
  if (live?.storage) {
    const used = live.storage.used / live.storage.total;
    checks.push(['Storage', used > 0.9 ? `${Math.round(used * 100)}% of Android’s storage is used` : '', ['storage', 'Open Storage']]);
  }
  if (host) {
    checks.push(
      ['PC VR runtime', host.openxr ? '' : 'No OpenXR runtime; VR play needs Meta Horizon Link or SteamVR'],
      ['Acceleration', !host.acceleration || host.acceleration.ok ? '' : host.acceleration.detail],
    );
  }
  if (guest?.incomplete) checks.push(['Snapshot', `Android stopped answering after “${guest.incomplete.stoppedAfter}”`]);
  return checks;
}

function Overview({ state, status, info, loadInfo, pending, run, notify, action, busy, game, onLogcat, onStorage }) {
  const [shot, setShot] = useState(null);
  const online = status?.state === 'running';
  const guest = info?.guest, host = info?.host, qemu = status?.qemu, live = status?.guest;
  const props = guest?.props || {};
  const summary = () => JSON.stringify({ status, info: info && { ...info, guest: guest && { ...guest, props: undefined } } }, null, 2);
  const blocked = busy || Boolean(state.running);
  const history = status?.history || [];
  const checks = healthChecks({ state, status, info });
  const problems = checks.filter(c => c[1]);
  const fix = async ([kind, , message]) => {
    if (kind === 'storage') return onStorage();
    if (kind === 'restart') {
      await action('stop', null);
      return action('start', null, () => notify('Emulator restarted'));
    }
    return action(kind, null, () => notify(kind === 'prepare' ? 'Android is set up for Refract' : message));
  };
  const processes = guest ? parseTop(guest.top) : [];
  const storage = live?.storage;

  return <div className="emu-overview">
    <div className="tiles">
      <Tile label="Android" value={status ? stateLabels[status.state] : 'Checking'} sub={live?.uptime ? `Up ${duration(live.uptime)}` : status?.startedHere ? 'Started by Refract' : undefined} tone={online ? 'good' : ''} />
      <Tile label="Emulator CPU" value={qemu?.cpuPercent != null ? `${qemu.cpuPercent.toFixed(0)}%` : qemu ? '…' : '—'} sub={qemu ? `of this PC · ${qemu.cores} vCPU${qemu.cores === 1 ? '' : 's'}` : undefined}
        chart={qemu ? history.map(h => (h.cpu == null ? null : h.cpu / 100)) : null} />
      <Tile label="Android memory" value={live?.memTotal ? bytes(live.memTotal - live.memAvailable) : '—'} sub={live?.memTotal ? `of ${bytes(live.memTotal)}` : undefined}
        chart={live?.memTotal ? history.map(h => h.memory) : null} />
      <Tile label="Android storage" value={storage ? bytes(storage.used) : '—'} sub={storage ? `of ${bytes(storage.total)}` : undefined}
        meter={storage ? storage.used / storage.total : null} tone={storage && storage.used / storage.total > 0.9 ? 'warn' : ''} />
      <Tile label="Emulator on this PC" value={qemu ? bytes(qemu.memory) : '—'} sub={qemu ? 'memory in use' : undefined} />
    </div>

    <div className="tools">
      <button type="button" className="btn btn-outline btn-sm" disabled={!online || pending.has('emu-screenshot')} onClick={() => action('screenshot', null, setShot)}><Camera />Screenshot</button>
      <button type="button" className="btn btn-outline btn-sm" disabled={!online || blocked} onClick={() => action('prepare', null, () => notify('Refract components are up to date on Android'))}><PackageCheck />Reinstall Refract components</button>
      <button type="button" className="btn btn-outline btn-sm" disabled={pending.has('diagnostics')} onClick={() => run('diagnostics', async () => { await call('exportDiagnostics'); notify('Diagnostics saved (zip and folder)'); })}>
        {pending.has('diagnostics') ? <Loader2 className="spin" /> : <FileArchive />}Export diagnostics</button>
      <div className="grow" />
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Reconnect adb" title="Reconnect adb" disabled={pending.has('emu-reconnect')} onClick={() => action('reconnect', null, () => notify('Reconnected offline devices'))}><Plug /></button>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Restart adb server" title="Restart adb server" disabled={blocked} onClick={() => action('restartAdb', null, () => notify('adb server restarted'))}><Server /></button>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Emulator logs" title="Open the emulator logs folder" onClick={() => run('open-logs', () => call('openLogs', 'emulator'))}><FolderOpen /></button>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Copy details" title="Copy these details" disabled={!info} onClick={() => copy(summary(), notify)}><Copy /></button>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Refresh" title="Refresh" disabled={pending.has('emulator-info')} onClick={loadInfo}>{pending.has('emulator-info') ? <Loader2 className="spin" /> : <RefreshCw />}</button>
    </div>

    {shot && <section className="panel shot">
      <div className="panel-row"><h2>Screenshot</h2><div className="tools">
        <button type="button" className="btn btn-ghost btn-sm" onClick={() => run('open-shots', () => call('openLogs', 'screenshots'))}><FolderOpen />Open folder</button>
        <button type="button" className="icon-btn icon-btn-sm" aria-label="Close screenshot" onClick={() => setShot(null)}><X /></button></div></div>
      <img src={shot.image} alt="What Android shows now" /><small className="mono muted">{shot.path}</small>
    </section>}

    {checks.length > 0 && <section className={`panel checks ${problems.length ? 'has-problems' : ''}`}>
      <div className="panel-row">
        <h2>{problems.length ? `${problems.length} problem${problems.length === 1 ? '' : 's'}` : 'Everything looks good'}</h2>
        <span className="muted">{checks.length - problems.length} of {checks.length} checks passed</span>
      </div>
      {problems.length > 0 && <ul className="problem-list">{problems.map(([label, problem, todo]) => <li key={label}>
        <TriangleAlert /><div><strong>{label}</strong><span>{problem}</span></div>
        {todo && <button type="button" className="btn btn-outline btn-sm" title={todo[2]} disabled={todo[0] !== 'storage' && (blocked || pending.has(`emu-${todo[0]}`))} onClick={() => fix(todo)}>{todo[1]}</button>}
      </li>)}</ul>}
      <div className="check-chips">{checks.filter(c => !c[1]).map(([label]) => <span key={label}><CircleCheck />{label}</span>)}</div>
    </section>}

    <div className="info-grid">
      <div className="info-col">
      <section className="panel">
        <h2>Android</h2>
        {!guest ? <p>{online ? 'Reading Android…' : 'Start the emulator to see Android’s details.'}</p> : <KeyValues rows={[
          ['Device', `${props['ro.product.manufacturer']} ${props['ro.product.model']}`],
          ['Android', `${props['ro.build.version.release']} (API ${props['ro.build.version.sdk']})`],
          ['Graphics', (guest.gles || '').replace(/^[^,]+,\s*/, '').replace(/\/PCIe\/SSE2/, '')],
          ['CPUs', `${guest.cores}${live?.load?.length ? ` · load ${live.load.map(n => n.toFixed(1)).join(' ')}` : ''}`],
          ['Memory', `${bytes(guest.memory.available)} free of ${bytes(guest.memory.total)}${guest.memory.swapTotal ? ` · swap ${bytes(guest.memory.swapTotal - guest.memory.swapFree)} used` : ''}`],
          ['Display', (guest.display || '').replace(/Physical size: /, '').replace(/Physical density: (\d+)/, '$1 dpi')],
          ['Foreground', (guest.resumed || '').split('/')[0], 'mono'],
        ]} />}
      </section>

      <section className="panel">
        <h2>Refract components</h2>
        {!info ? <p>Checking…</p> : <KeyValues rows={info.components.map(c => {
          const installed = c.package && guest?.guestPackages.find(g => g.package === c.package);
          const onAndroid = !c.package || !guest ? '' : !installed?.installed ? bad('Not installed') : installed.sha256 === c.sha256 ? 'Up to date' : bad('Older build');
          return [c.label, <span className="kv-split">{c.built ? <span className="muted">Built {ago(c.modified)}</span> : bad('Not built')}{onAndroid}</span>];
        })} />}
      </section>

      {processes.length > 0 && <section className="panel">
        <h2>Android processes</h2>
        <table className="table proc-table">
          <thead><tr><th>Process</th><th className="num" title="100% is one Android CPU">CPU</th><th className="num">Memory</th></tr></thead>
          <tbody>{processes.map(p => <tr key={p.pid}><td title={`${p.name} · PID ${p.pid} · ${p.user}`}><span className="proc-name">{p.name}</span></td>
            <td className="num">{p.cpu.toFixed(0)}%</td><td className="num">{p.res ? bytes(p.res) : '—'}</td></tr>)}</tbody>
        </table>
      </section>}
      </div>

      <div className="info-col">
      <section className="panel">
        <h2>Emulator</h2>
        {!qemu ? <p>{status ? 'No emulator is running this virtual device.' : 'Checking…'}</p> : <KeyValues rows={[
          ['qemu', `${qemu.multicore ? 'Multi-core' : 'Stock'} · ${qemu.cores} vCPU${qemu.cores === 1 ? '' : 's'} · ${number(qemu.memoryMB || state.settings.memoryMB)} MB`],
          ['Window', qemu.window ? 'Shown' : 'Hidden'],
          ['Started', qemu.started && ago(qemu.started)],
        ]} />}
        {status?.processes.length > 0 && <table className="table proc-table">
          <thead><tr><th>Process</th><th className="num">CPU</th><th className="num">Memory</th></tr></thead>
          <tbody>{status.processes.map(p => <tr key={p.pid}><td title={`PID ${p.pid}`}><span className="proc-name">{p.name}</span></td>
            <td className="num">{p.cpuPercent != null ? `${p.cpuPercent.toFixed(1)}%` : '…'}</td><td className="num">{bytes(p.memory)}</td></tr>)}</tbody>
        </table>}
      </section>

      <section className="panel">
        <h2>This PC</h2>
        {!host ? <p>Checking…</p> : <KeyValues rows={[
          ['CPU', host.cpu && `${host.cpu} (${host.threads} threads)`],
          ['Memory', host.memory && `${bytes(host.free)} free of ${bytes(host.memory)}`],
          ['Graphics', host.gpus?.length ? <>{host.gpus.map(g => <div key={g}>{g.replace(/ \(driver .*\)$/, '')}</div>)}</> : ''],
          ['VR runtime', host.openxr ? host.openxr.split('\\').at(-1).replace(/\.json$/, '') : bad('None')],
        ]} />}
      </section>

      {guest && <section className="panel">
        <div className="panel-row"><h2>Installed apps</h2><span className="muted">{guest.packages.length}</span></div>
        <ul className="app-list">
          {guest.packages.map(p => <li key={p.package}>
            <div><span className="mono">{p.package}</span>{game?.package === p.package && <span className="tag">Running</span>}</div>
            <button type="button" className="btn btn-ghost btn-sm" onClick={() => onLogcat(p.package)}><ScrollText />Logcat</button>
            <button type="button" className="btn btn-ghost btn-sm" disabled={pending.has('emu-forceStop')} onClick={() => action('forceStop', p.package, () => notify(`Stopped ${p.package}`))}><Square />Force stop</button>
          </li>)}
        </ul>
      </section>}
      </div>
    </div>

    {info && <details className="panel props">
      <summary><h2>Technical details</h2></summary>
      <div className="details-grid">
        {guest && <KeyValues rows={[
          ['Build', props['ro.build.fingerprint'], 'mono'],
          ['ABIs', props['ro.product.cpu.abilist'], 'mono'],
          ['ARM translator', props['ro.dalvik.vm.native.bridge'], 'mono'],
          ['OpenGL ES', guest.gles],
          ['Vulkan driver', props['ro.hardware.vulkan']],
          ['Clock source', guest.clocksource],
          ['SELinux', guest.selinux],
          ['Focused window', guest.focus, 'mono'],
          ['Kernel', guest.kernel, 'mono'],
          ['Kernel command line', guest.cmdline, 'mono'],
          ...Object.entries(props).filter(([k]) => /refract/.test(k)).map(([k, v]) => [k, v || '(empty)', 'mono']),
        ]} />}
        <KeyValues rows={[
          qemu && ['qemu', `${qemu.path} (PID ${qemu.pid})`, 'mono'],
          qemu && ['Command line', status.processes.find(p => p.pid === qemu.pid)?.command, 'mono small'],
          host && ['Windows', host.os],
          host && ['OpenXR runtime', host.openxr, 'mono'],
          host && ['Acceleration', host.acceleration?.detail],
          host && ['Android SDK', host.sdk, 'mono'],
          host && ['Emulator', `${host.emulatorVersion || 'not installed'}${host.multicoreQemu ? ' · multi-core qemu' : ''}`],
          host && ['adb', host.adbVersion],
          host && ['Node.js', host.node],
          host && ['Launcher', host.launcherVersion],
          host && ['Refract commit', host.commit, 'mono'],
          host && ['Refract folder', host.root, 'mono'],
          host && ['Launcher data', host.data, 'mono'],
        ]} />
      </div>
    </details>}
    {guest && <Properties props={props} />}
  </div>;
}

// A button that asks once more ("click again") before doing something that cannot be undone.
function ConfirmButton({ confirm, onConfirm, className = '', children, ...props }) {
  const [armed, setArmed] = useState(false);
  useEffect(() => {
    if (!armed) return undefined;
    const timer = setTimeout(() => setArmed(false), 4000);
    return () => clearTimeout(timer);
  }, [armed]);
  return <button type="button" className={`${className} ${armed ? 'armed' : ''}`} {...props} onClick={() => { if (armed) { setArmed(false); onConfirm(); } else setArmed(true); }}>
    {armed ? confirm : children}</button>;
}

const sum = (list, key) => list.reduce((n, item) => n + (item[key] || 0), 0);

function Storage({ state, status, run, pending, notify, action, busy }) {
  const [data, setData] = useState(null);
  const [selected, setSelected] = useState(() => new Set());
  const [size, setSize] = useState(0);
  const online = status?.state === 'running', up = status && status.state !== 'stopped';
  const load = useCallback(() => run('emu-storage', async () => setData(await call('emulatorStorage'))), [run]);
  useEffect(() => { if (status) load(); }, [load, Boolean(status), online]); // eslint-disable-line react-hooks/exhaustive-deps
  const act = async (name, arg, message) => { await action(name, arg, () => notify(message)); load(); };

  const guest = data?.guest, host = data?.host;
  const leftovers = guest?.leftovers || [];
  // Selections only of files that are still there.
  const chosen = leftovers.filter(f => selected.has(f.path));
  const toggle = file => setSelected(old => { const next = new Set(old); if (next.has(file)) next.delete(file); else next.add(file); return next; });
  const gameRunning = Boolean(state.running);
  const blocked = busy || gameRunning;
  // One size for both disk actions: Grow uses it when it is larger, Reset makes the new disk this size.
  const currentSize = host?.dataSize ? Math.round(host.dataSize / 1024 ** 3) : 0;
  const target = size || currentSize || 64;
  const sizes = [...new Set([...(host?.sizes || []), currentSize].filter(Boolean))].sort((a, b) => a - b);
  const short = host && target * 1024 ** 3 - host.dataDisk > host.driveFree;

  const parts = guest && (() => {
    const apps = guest.apps.reduce((n, a) => n + a.apk + a.data + a.external, 0), obb = sum(guest.apps, 'obb'), extra = sum(leftovers, 'size');
    return [['Game files', obb, 'seg-obb'], ['Apps and data', apps, 'seg-apps'], ['Leftovers', extra, 'seg-left'],
      ['System and other', Math.max(0, guest.used - apps - obb - extra), 'seg-other']];
  })();

  return <div className="emu-storage">
    <section className="panel">
      <div className="panel-row">
        <h2>{guest ? <>{bytes(guest.used)} <span className="muted">of {bytes(guest.total)} used</span></> : 'Android storage'}</h2>
        <div className="panel-actions">
          {guest && <span className={guest.used / guest.total > 0.85 ? 'bad' : 'muted'}>{bytes(guest.free)} free</span>}
          <button type="button" className="icon-btn icon-btn-sm" aria-label="Refresh" title="Refresh" disabled={pending.has('emu-storage')} onClick={load}>
            {pending.has('emu-storage') ? <Loader2 className="spin" /> : <RefreshCw />}</button>
        </div>
      </div>
      {!guest ? <p className="hint">{online ? 'Reading Android…' : up ? 'Android is starting.' : 'Start the emulator to see what is on Android.'}</p> : <>
        <div className="usage-bar" role="img" aria-label={parts.map(([label, n]) => `${label} ${bytes(n)}`).join(', ')}>
          {parts.map(([label, n, cls]) => n > 0 && <div key={label} className={cls} style={{ width: `${(n / guest.total) * 100}%` }} title={`${label}: ${bytes(n)}`} />)}
        </div>
        <ul className="usage-legend">{parts.map(([label, n, cls]) => <li key={label}><span className={`swatch ${cls}`} />{label}<strong>{bytes(n)}</strong></li>)}</ul>
      </>}
    </section>

    {guest && <section className="panel">
      <h2>Games and apps</h2>
      <table className="table storage-table">
        <thead><tr><th>App</th><th className="num">App</th><th className="num">Data</th><th className="num">Game files</th><th className="num">Total</th><th /></tr></thead>
        <tbody>{guest.apps.map(a => {
          const running = data.running === a.package, name = a.name || a.package;
          return <tr key={a.package}>
            <td><div>{name}{a.component && <span className="tag">Refract</span>}{running && <span className="tag ok">Running</span>}</div>{a.name && <small className="mono muted">{a.package}</small>}</td>
            <td className="num">{bytes(a.apk)}</td>
            <td className="num" title={`Cache ${bytes(a.cache)}`}>{bytes(a.data + a.external)}</td>
            <td className="num">{a.obb > 64 * 1024 ? bytes(a.obb) : <span className="muted">—</span>}</td>
            <td className="num"><strong>{bytes(a.total)}</strong></td>
            <td className="row-actions">
              <button type="button" className="btn btn-ghost btn-sm" disabled={running || busy || pending.has('emu-clearCache')} title={`Cache: ${bytes(a.cache)}`}
                onClick={() => act('clearCache', a.package, `Cleared the cache of ${name}`)}><Eraser />Clear cache</button>
              {!a.component && <>
                <ConfirmButton className="btn btn-ghost btn-sm" confirm="Delete saves?" disabled={running || busy || pending.has('emu-clearData')} title="Deletes saves, settings and sign-ins"
                  onConfirm={() => act('clearData', a.package, `Cleared the data of ${name}`)}><Trash2 />Clear data</ConfirmButton>
                <ConfirmButton className="btn btn-ghost btn-sm" confirm="Uninstall?" disabled={running || blocked || pending.has('emu-uninstall')}
                  onConfirm={() => act('uninstall', a.package, `Uninstalled ${name}`)}><PackageX />Uninstall</ConfirmButton>
              </>}
            </td>
          </tr>;
        })}</tbody>
      </table>
    </section>}

    <div className="storage-cols">
      {leftovers.length > 0 && <section className="panel">
        <div className="panel-row">
          <h2>Leftover files <span className="muted">{bytes(sum(leftovers, 'size'))}</span></h2>
          <ConfirmButton className="btn btn-outline btn-sm" confirm={`Delete ${chosen.length}?`} disabled={!chosen.length || busy || pending.has('emu-deleteFiles')}
            onConfirm={async () => { await act('deleteFiles', chosen.map(f => f.path), `Deleted ${bytes(sum(chosen, 'size'))}`); setSelected(new Set()); }}>
            <Trash2 />Delete{chosen.length ? ` ${bytes(sum(chosen, 'size'))}` : ''}</ConfirmButton>
        </div>
        <ul className="app-list file-list">
          <li><label className="check"><input type="checkbox" checked={chosen.length === leftovers.length} onChange={e => setSelected(e.target.checked ? new Set(leftovers.map(f => f.path)) : new Set())} />
            <span className="muted">Select all</span></label></li>
          {leftovers.map(f => <li key={f.path}>
            <label className="check" title={f.path}><input type="checkbox" checked={selected.has(f.path)} onChange={() => toggle(f.path)} />
              <span className="mono">{f.name}</span></label>
            <span className="size">{bytes(f.size)}</span>
          </li>)}
        </ul>
      </section>}

      {host && <section className="panel">
        <h2>Disk</h2>
        {!host.exists ? <p className="hint">No virtual device named {state.settings.avd}. Settings &gt; Setup creates it.</p> : <>
          <KeyValues rows={[
            ['Android’s disk', bytes(host.dataSize)],
            ['File on this PC', bytes(host.total)],
            ['Drive', `${bytes(host.driveFree)} free`, host.driveFree < 20 * 1024 ** 3 ? 'bad' : ''],
            host.snapshots > 0 && ['Saved snapshot', <span className="kv-action">{bytes(host.snapshots)}
              <ConfirmButton className="btn btn-ghost btn-sm" confirm="Delete?" disabled={up || busy || pending.has('emu-deleteSnapshots')} title={up ? 'Stop the emulator first' : 'Never used: Refract always starts Android fresh'}
                onConfirm={() => act('deleteSnapshots', null, `Deleted the saved snapshot (${bytes(host.snapshots)})`)}><Trash2 />Delete</ConfirmButton></span>],
            host.growPending && ['Bigger disk', bad('Pending', 'Finishes the next time Android starts')],
            host.resetPending && ['Reset', bad('Pending', 'Happens the next time Android starts')],
          ]} />
          <div className="disk-controls">
            <select className="field-input compact" aria-label="Disk size" value={target} onChange={e => setSize(Number(e.target.value))}>
              {sizes.map(n => <option key={n} value={n}>{n} GB{n === currentSize ? ' (now)' : ''}</option>)}
            </select>
            <ConfirmButton className="btn btn-outline btn-sm" confirm={`Grow to ${target} GB?`} disabled={target <= currentSize || blocked}
              title={target <= currentSize ? 'Choose a larger size' : 'Keeps every game and save; Android restarts twice'}
              onConfirm={() => act('growDisk', target, `Android’s disk is now ${target} GB`)}><HardDrive />Grow</ConfirmButton>
            <ConfirmButton className="btn btn-outline btn-sm btn-danger-outline" confirm="Erase everything?" disabled={blocked}
              title={`Deletes every game, save and setting on Android; the new disk is ${target} GB`}
              onConfirm={() => act('resetAndroid', target, 'Android was reset')}><RotateCcw />Reset Android</ConfirmButton>
          </div>
          {short && <p className="hint bad">The disk file can need {bytes(target * 1024 ** 3 - host.dataDisk)} more, but the drive has {bytes(host.driveFree)} free.</p>}
        </>}
      </section>}
    </div>
  </div>;
}

function Properties({ props }) {
  const matcher = useMatcher();
  const rows = Object.entries(props).filter(([k, v]) => matcher.test(`${k} ${v}`));
  return <details className="panel props">
    <summary><h2>All system properties</h2><span className="muted">{Object.keys(props).length}</span></summary>
    <div className="toolbar"><SearchBox matcher={matcher} placeholder="Filter properties" /></div>
    <table className="table mono"><tbody>{rows.map(([k, v]) => <tr key={k}><td><Highlight text={k} pattern={matcher.pattern} /></td><td><Highlight text={v} pattern={matcher.pattern} /></td></tr>)}</tbody></table>
  </details>;
}

function Logcat({ logcat, info, game, app, setApp, run, notify, status }) {
  const matcher = useMatcher();
  const [level, setLevelState] = useState(() => remembered('level', 'V'));
  const setLevel = value => { setLevelState(value); remember('level', value); };
  const [tag, setTag] = useState('');
  const [pid, setPid] = useState(0);
  const [follow, setFollow] = useState(true);
  const [selected, setSelected] = useState(null);
  const pids = usePids(app);
  const min = levels.indexOf(level);
  const rows = useMemo(() => logcat.entries.filter(e => (e[4] === '-' || levels.indexOf(e[4]) >= min)
    && (!tag || e[5] === tag) && (!pid || e[2] === pid) && (!app || pids.includes(e[2]))
    && (!matcher.active || matcher.test(`${e[5]}: ${e[6]}`))),
  // eslint-disable-next-line react-hooks/exhaustive-deps
  [logcat.version, min, tag, pid, app, pids, matcher.query, matcher.regex, matcher.matchCase]);

  const apps = [...new Set([game?.package, ...(info?.guest?.packages.map(p => p.package) || []), 'com.oculus.horizon', 'com.oculus.systemdriver', 'com.refract.openxrruntime'].filter(Boolean))];
  if (app && !apps.includes(app)) apps.unshift(app);
  const exportRows = () => run('logcat-export', async () => { await call('logcatExport', rows.map(formatEntry).join('\n')); notify(`Saved ${number(rows.length)} lines`); });

  const renderRow = (i, style) => {
    const e = rows[i];
    return <div key={e[0]} style={style} className={`log-row lv-${e[4]} ${selected?.[0] === e[0] ? 'selected' : ''}`} onClick={() => setSelected(e)}>
      <span className="c-time">{e[1]}</span>
      <span className="c-pid">{e[2] ? `${e[2]} ${e[3]}` : ''}</span>
      <span className="c-lv">{e[4] === '-' ? '' : e[4]}</span>
      <span className="c-tag" title={e[5]}><Highlight text={e[5]} pattern={matcher.pattern} /></span>
      <span className="c-msg"><Highlight text={e[6]} pattern={matcher.pattern} /></span>
    </div>;
  };
  const filtered = matcher.active || tag || pid || app || min > 0;

  return <div className="log-pane">
    <div className="log-toolbar">
      <SearchBox matcher={matcher} placeholder="Search tag or message" label="Search logcat" />
      <select className="field-input compact" aria-label="Minimum level" value={level} onChange={e => setLevel(e.target.value)}>
        {levels.map(l => <option key={l} value={l}>{levelNames[l]}{l === 'V' ? ' (all)' : ' and above'}</option>)}
      </select>
      <select className="field-input compact app-select" aria-label="App" value={app} onChange={e => setApp(e.target.value)}>
        <option value="">All apps</option>
        {apps.map(p => <option key={p} value={p}>{p}{p === game?.package ? ' (running)' : ''}</option>)}
      </select>
      <div className="grow" />
      <button type="button" className="icon-btn icon-btn-sm" aria-label={logcat.paused ? 'Resume' : 'Pause'} title={logcat.paused ? 'Resume' : 'Pause'} onClick={() => logcat.setPaused(p => !p)}>{logcat.paused ? <Play /> : <Pause />}</button>
      <button type="button" className="icon-btn icon-btn-sm" aria-pressed={follow} aria-label="Follow new lines" title="Follow new lines" onClick={() => setFollow(f => !f)}><ChevronsDown /></button>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Save visible lines" title="Save visible lines to a file" disabled={!rows.length} onClick={exportRows}><Download /></button>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Clear logcat" title="Clear logcat (on Android too)" onClick={() => run('logcat-clear', logcat.clear)}><Trash2 /></button>
    </div>
    <div className="log-status">
      {app && <Chip onRemove={() => setApp('')}>App {app}{pids.length ? ` · PID ${pids.join(', ')}` : ' · not running'}</Chip>}
      {tag && <Chip onRemove={() => setTag('')}>Tag {tag}</Chip>}
      {pid > 0 && <Chip onRemove={() => setPid(0)}>PID {pid}</Chip>}
      <span className="muted">{filtered ? `${number(rows.length)} of ${number(logcat.entries.length)} lines` : `${number(logcat.entries.length)} lines`}
        {' · '}{logcat.paused ? 'Paused' : logcat.meta.running ? `Streaming from ${logcat.meta.serial}` : status?.state === 'running' ? 'Connecting…' : 'Android is not running'}</span>
      {matcher.error && <span className="bad">{matcher.error}</span>}
      {logcat.meta.error && !logcat.meta.running && status?.state === 'running' && <span className="bad">{logcat.meta.error}</span>}
    </div>
    <LogList count={rows.length} renderRow={renderRow} follow={follow && !logcat.paused} setFollow={setFollow} label="Logcat"
      empty={logcat.entries.length ? 'No lines match these filters.' : status?.state === 'running' ? 'Waiting for logcat…' : 'Start the emulator to see logcat.'} />
    {selected && <div className="log-detail">
      <div className="log-detail-head">
        <span className="mono">{selected[1]} · PID {selected[2]} · TID {selected[3]} · {levelNames[selected[4]] || selected[4]} · {selected[5]}</span>
        <div className="grow" />
        {selected[5] && <button type="button" className="btn btn-ghost btn-sm" onClick={() => setTag(selected[5])}>Only this tag</button>}
        {selected[2] > 0 && <button type="button" className="btn btn-ghost btn-sm" onClick={() => setPid(selected[2])}>Only this PID</button>}
        <button type="button" className="btn btn-ghost btn-sm" onClick={() => copy(formatEntry(selected), notify)}><Copy />Copy</button>
        <button type="button" className="icon-btn icon-btn-sm" aria-label="Close" onClick={() => setSelected(null)}><X /></button>
      </div>
      <pre className="selectable">{selected[6]}</pre>
    </div>}
  </div>;
}

function LogFiles({ run, pending, notify }) {
  const [files, setFiles] = useState([]);
  const [source, setSourceState] = useState(() => remembered('log', 'emulator-err'));
  const setSource = value => { setSourceState(value); remember('log', value); };
  const [file, setFile] = useState(null);
  const [live, setLive] = useState(false);
  const [follow, setFollow] = useState(true);
  const [onlyMatches, setOnlyMatches] = useState(true);
  const matcher = useMatcher();
  const load = useCallback(async () => {
    const [list, content] = await Promise.all([call('logFiles'), call('logFile', source)]);
    setFiles(list); setFile(content);
  }, [source]);
  useEffect(() => { run('log-file', load); }, [run, load]);
  useEffect(() => {
    if (!live) return undefined;
    const timer = setInterval(() => load().catch(() => {}), 2000);
    return () => clearInterval(timer);
  }, [live, load]);
  const lines = useMemo(() => (file?.text || '').replace(/\r?\n$/, '').split(/\r?\n/).map((text, i) => [i + 1, text]).filter(([, text], i, all) => all.length > 1 || text), [file]);
  const rows = useMemo(() => (onlyMatches && matcher.active ? lines.filter(([, text]) => matcher.test(text)) : lines),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [lines, onlyMatches, matcher.query, matcher.regex, matcher.matchCase]);
  const renderRow = (i, style) => {
    const [n, text] = rows[i];
    return <div key={n} style={style} className="log-row plain"><span className="c-ln">{n}</span><span className="c-msg"><Highlight text={text} pattern={matcher.pattern} /></span></div>;
  };
  return <div className="log-pane">
    <div className="log-toolbar">
      <select className="field-input compact" aria-label="Log file" value={source} onChange={e => setSource(e.target.value)}>
        {files.map(f => <option key={f.id} value={f.id}>{f.label}{f.exists ? ` · ${f.size ? bytes(f.size) : 'empty'} · ${ago(f.modified)}` : ' · not written yet'}</option>)}
      </select>
      <SearchBox matcher={matcher} placeholder="Search this log" label="Search log file" />
      <label className="check"><input type="checkbox" checked={onlyMatches} onChange={e => setOnlyMatches(e.target.checked)} />Only matching lines</label>
      <div className="grow" />
      <label className="check"><input type="checkbox" checked={live} onChange={e => setLive(e.target.checked)} />Live</label>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Reload" title="Reload" disabled={pending.has('log-file')} onClick={() => run('log-file', load)}><RefreshCw /></button>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Copy log" title="Copy visible lines" disabled={!rows.length} onClick={() => copy(rows.map(r => r[1]).join('\n'), notify)}><Copy /></button>
      <button type="button" className="icon-btn icon-btn-sm" aria-label="Open folder" title="Open the folder" onClick={() => run('open-logs', () => call('openLogs', source.startsWith('emulator') ? 'emulator' : source === 'launcher' ? 'data' : 'game'))}><FolderOpen /></button>
    </div>
    <div className="log-status">
      <span className="muted mono">{file?.file}</span>
      {file?.exists && <span className="muted">{matcher.active && onlyMatches ? `${number(rows.length)} of ${number(lines.length)} lines` : `${number(lines.length)} lines`}{file.truncated ? ' · last 4 MB' : ''} · updated {ago(file.modified)}</span>}
      {matcher.error && <span className="bad">{matcher.error}</span>}
    </div>
    <LogList count={rows.length} renderRow={renderRow} follow={follow} setFollow={setFollow} label="Log file"
      empty={!file ? 'Loading…' : !file.exists ? 'This log has not been written yet. It appears after the emulator or a game session runs.' : lines.length ? 'No lines match.' : 'This log is empty.'} />
  </div>;
}

const quickCommands = [
  ['Refract properties', 'getprop | grep -i refract'],
  ['Top processes', 'top -b -n 1 -m 15'],
  ['Foreground app', 'dumpsys activity activities | grep -E "mResumedActivity|topResumedActivity"'],
  ['Memory', 'cat /proc/meminfo | head -8'],
  ['GPU', 'dumpsys SurfaceFlinger | grep -m3 -E "GLES|Vulkan"'],
  ['Storage', 'df -h /data /sdcard'],
  ['Clock source', 'cat /sys/devices/system/clocksource/clocksource0/current_clocksource'],
  ['Third-party apps', 'pm list packages -3'],
];

function Shell({ online }) {
  const [blocks, setBlocks] = useState([]);
  const [input, setInput] = useState('');
  const [runningCommand, setRunning] = useState(false);
  const history = useRef([]);
  const position = useRef(-1);
  const out = useRef(null);
  useEffect(() => { out.current?.scrollTo({ top: out.current.scrollHeight }); }, [blocks]);
  const exec = async command => {
    command = command.trim();
    if (!command || runningCommand) return;
    if (command === 'clear') { setBlocks([]); setInput(''); return; }
    history.current = [command, ...history.current.filter(c => c !== command)].slice(0, 100); position.current = -1;
    const id = Date.now();
    setBlocks(b => [...b.slice(-100), { id, command, running: true }]); setInput(''); setRunning(true);
    try {
      const result = await call('adbShell', command);
      setBlocks(b => b.map(x => (x.id === id ? { ...x, ...result, running: false } : x)));
    } catch (e) {
      setBlocks(b => b.map(x => (x.id === id ? { ...x, output: errorText(e), code: -1, running: false } : x)));
    } finally { setRunning(false); }
  };
  const onKey = e => {
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      e.preventDefault();
      const next = Math.max(-1, Math.min(history.current.length - 1, position.current + (e.key === 'ArrowUp' ? 1 : -1)));
      position.current = next; setInput(next < 0 ? '' : history.current[next]);
    }
  };
  return <div className="log-pane">
    <div className="quick">{quickCommands.map(([label, command]) => <button key={label} type="button" className="tag-btn" disabled={!online || runningCommand} title={command} onClick={() => exec(command)}>{label}</button>)}</div>
    <div className="shell-out selectable" ref={out}>
      {!blocks.length && <div className="log-empty">{online ? 'Runs commands in Android’s shell (adb shell). ↑ and ↓ go through earlier commands; “clear” empties this view.' : 'Start the emulator to use the shell.'}</div>}
      {blocks.map(b => <div key={b.id} className="shell-block">
        <div className="shell-cmd"><span>$</span>{b.command}{!b.running && <small className={b.code ? 'bad' : 'muted'}>{b.code ? `exit ${b.code}` : 'ok'} · {b.ms} ms</small>}</div>
        {b.running ? <Loader2 className="spin" /> : <pre>{b.output || <span className="muted">(no output)</span>}{b.truncated && <span className="muted">{'\n'}[output cut off]</span>}</pre>}
      </div>)}
    </div>
    <form className="shell-input" onSubmit={e => { e.preventDefault(); exec(input); }}>
      <span className="mono">$</span>
      <input className="field-input mono" aria-label="Shell command" placeholder={online ? 'getprop ro.product.model' : 'Android is not running'} value={input} disabled={!online}
        onChange={e => setInput(e.target.value)} onKeyDown={onKey} spellCheck={false} autoComplete="off" />
      <button type="submit" className="btn btn-primary" disabled={!online || runningCommand || !input.trim()}>{runningCommand ? <Loader2 className="spin" /> : <CornerDownLeft />}Run</button>
    </form>
  </div>;
}

function EmulatorSettings({ state, run, pending, notify, up }) {
  const { draft, edit, field, toggle, save, saveBar } = useSettingsForm({ state, run, pending, notify });
  return <form className="settings" onSubmit={save}>
    {up && <p className="note">Changes apply the next time the emulator starts.</p>}
    <section className="panel">
      <h2>Virtual device</h2>
      {field('sdk', 'Android SDK', { required: true }, true)}
      {field('avd', 'Virtual device', { required: true, pattern: '[a-zA-Z0-9_\\-]+' })}
      <div className="fields-2">
        {field('port', 'Port', { type: 'number', min: 5554, max: 5682, step: 2, required: true })}
        {field('memoryMB', 'Memory (MB)', { type: 'number', min: 2048, max: 16384, step: 1024, required: true })}
      </div>
      <div className="fields-2">
        <div className="field">
          <label htmlFor="cores">CPU cores</label>
          <select id="cores" className="field-input" value={draft.cores} onChange={e => edit('cores', Number(e.target.value))}
            title="Refract uses at most half of this PC’s performance-core threads">
            {[1, 2, 3, 4, 5, 6].map(n => <option key={n} value={n}>{n}{n === 6 ? ' (default)' : n === 1 ? ' (stock emulator)' : ''}</option>)}
          </select>
        </div>
        <div className="field">
          <label htmlFor="audio">Audio output</label>
          <select id="audio" className="field-input" value={draft.audio} onChange={e => edit('audio', e.target.value)}>
            <option value="dsound">DirectSound (default)</option>
            <option value="winaudio">Windows audio</option>
            <option value="sdl">SDL</option>
          </select>
        </div>
      </div>
    </section>
    <section className="panel">
      <h2>Options</h2>
      {toggle('hostMic', 'Microphone', 'For voice chat in games.')}
      {toggle('showWindow', 'Show the emulator window', 'Android’s own window, for debugging.')}
      {toggle('keepEmulator', 'Keep Android running when Refract closes', 'Makes the next launch faster.')}
    </section>
    {saveBar}
  </form>;
}
