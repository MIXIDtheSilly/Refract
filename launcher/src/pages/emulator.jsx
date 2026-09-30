import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Camera, ChevronsDown, Copy, CornerDownLeft, Download, FileArchive, FolderOpen, Loader2, PackageCheck, Pause, Play, Plug, Power, RefreshCw, RotateCcw, ScrollText, Square, Trash2, X } from 'lucide-react';
import { call } from '../api';
import { ago, bytes } from '../components/common';
import { Chip, Highlight, LogList, SearchBox, useMatcher } from '../components/log-view';
import { useSettingsForm } from '../components/settings-form';

const tabs = [['overview', 'Overview'], ['logcat', 'Logcat'], ['logs', 'Log files'], ['shell', 'Shell'], ['settings', 'Settings']];
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
function useStatus() {
  const [status, setStatus] = useState(null);
  const [error, setError] = useState('');
  const refresh = useCallback(async () => {
    try { setStatus(await call('emulatorStatus')); setError(''); } catch (e) { setError(errorText(e)); }
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
      onLogcat={pkg => { setApp(pkg); setTab('logcat'); }} />}
    {tab === 'logcat' && <Logcat {...{ logcat, info, game, app, setApp, run, notify, status }} />}
    {tab === 'logs' && <LogFiles run={run} pending={pending} notify={notify} />}
    {tab === 'shell' && <Shell online={online} />}
    {tab === 'settings' && <EmulatorSettings key={JSON.stringify(state.settings)} {...{ state, run, pending, notify, up }} />}
  </div>;
}

function Tile({ label, value, sub, tone }) {
  return <div className={`tile ${tone || ''}`}><small>{label}</small><strong>{value ?? '—'}</strong>{sub && <span>{sub}</span>}</div>;
}
function KeyValues({ rows }) {
  return <dl className="kv">{rows.filter(r => r && r[1] !== undefined && r[1] !== null && r[1] !== '').map(([key, value, className]) =>
    <div key={key}><dt>{key}</dt><dd className={className || ''}>{value}</dd></div>)}</dl>;
}
const bad = (value, reason) => <span className="bad">{value}{reason && <small> — {reason}</small>}</span>;

function Overview({ state, status, info, loadInfo, pending, run, notify, action, busy, game, onLogcat }) {
  const [shot, setShot] = useState(null);
  const online = status?.state === 'running';
  const guest = info?.guest, host = info?.host, qemu = status?.qemu, live = status?.guest;
  const props = guest?.props || {};
  const summary = () => JSON.stringify({ status, info: info && { ...info, guest: guest && { ...guest, props: undefined } } }, null, 2);

  const tiles = <div className="tiles">
    <Tile label="Android" value={status ? stateLabels[status.state] : 'Checking'} sub={live?.uptime ? `up ${duration(live.uptime)}` : status?.startedHere ? 'Started by Refract' : undefined} tone={online ? 'good' : ''} />
    <Tile label="Device" value={status?.serial || '—'} sub={status?.adopted ? `adopted · Refract is set to ${status.configuredPort}` : `port ${status?.configuredPort ?? '…'}`} />
    <Tile label="Emulator CPU" value={qemu?.cpuPercent != null ? `${qemu.cpuPercent.toFixed(1)}%` : qemu ? '…' : '—'} sub={qemu ? `of this PC · ${qemu.cores} vCPU${qemu.cores === 1 ? '' : 's'}${qemu.multicore ? '' : ' · stock qemu'}` : undefined} tone={qemu && qemu.cores === 1 ? 'warn' : ''} />
    <Tile label="Emulator memory" value={qemu ? bytes(qemu.memory) : '—'} sub={`${number(state.settings.memoryMB)} MB for Android`} />
    <Tile label="Android memory" value={live?.memTotal ? `${bytes(live.memTotal - live.memAvailable)} / ${bytes(live.memTotal)}` : '—'} sub={live?.memTotal ? `${Math.round((1 - live.memAvailable / live.memTotal) * 100)}% used` : undefined} />
    <Tile label="Load average" value={live?.load?.length ? live.load.map(n => n.toFixed(2)).join('  ') : '—'} sub={guest?.cores ? `${guest.cores} Android CPUs` : undefined} />
  </div>;

  const components = info?.components.map(c => {
    const installed = c.package && guest?.guestPackages.find(g => g.package === c.package);
    const onAndroid = !c.package ? '' : !guest ? 'Android not running' : !installed?.installed ? bad('Not installed', 'Play or “Reinstall Refract components” installs it')
      : installed.sha256 === c.sha256 ? `Up to date${installed.version ? ` · ${installed.version}` : ''}` : bad(`Differs from this build${installed.version ? ` · ${installed.version}` : ''}`, 'reinstalled before the next game');
    return [c.label, <>{c.built ? `Built ${ago(c.modified)}` : bad('Not built')}{onAndroid && <><br /><span className="muted">{onAndroid}</span></>}</>];
  }) || [];

  return <div className="emu-overview">
    {tiles}
    <div className="tools">
      <button type="button" className="btn btn-outline btn-sm" disabled={!online || pending.has('emu-screenshot')} onClick={() => action('screenshot', null, setShot)}><Camera />Screenshot</button>
      <button type="button" className="btn btn-outline btn-sm" disabled={!online || busy || Boolean(state.running)} onClick={() => action('prepare', null, () => notify('Refract components are up to date on Android'))}><PackageCheck />Reinstall Refract components</button>
      <button type="button" className="btn btn-outline btn-sm" disabled={pending.has('emu-reconnect')} onClick={() => action('reconnect', null, () => notify('Reconnected offline devices'))}><Plug />Reconnect adb</button>
      <button type="button" className="btn btn-outline btn-sm" disabled={busy || Boolean(state.running)} onClick={() => action('restartAdb', null, () => notify('adb server restarted'))}><RotateCcw />Restart adb server</button>
      <button type="button" className="btn btn-outline btn-sm" disabled={pending.has('diagnostics')} onClick={() => run('diagnostics', async () => { await call('exportDiagnostics'); notify('Diagnostics saved (zip and folder)'); })}>
        {pending.has('diagnostics') ? <Loader2 className="spin" /> : <FileArchive />}Export diagnostics</button>
      <button type="button" className="btn btn-ghost btn-sm" onClick={() => run('open-logs', () => call('openLogs', 'emulator'))}><FolderOpen />Emulator logs</button>
      <button type="button" className="btn btn-ghost btn-sm" disabled={!info} onClick={() => copy(summary(), notify)}><Copy />Copy details</button>
      <button type="button" className="btn btn-ghost btn-sm" disabled={pending.has('emulator-info')} onClick={loadInfo}>{pending.has('emulator-info') ? <Loader2 className="spin" /> : <RefreshCw />}Refresh</button>
    </div>
    {shot && <section className="panel shot">
      <div className="panel-row"><h2>Screenshot</h2><div className="tools">
        <button type="button" className="btn btn-ghost btn-sm" onClick={() => run('open-shots', () => call('openLogs', 'screenshots'))}><FolderOpen />Open folder</button>
        <button type="button" className="icon-btn icon-btn-sm" aria-label="Close screenshot" onClick={() => setShot(null)}><X /></button></div></div>
      <img src={shot.image} alt="What Android shows now" /><small className="mono muted">{shot.path}</small>
    </section>}

    <div className="info-grid">
      <section className="panel">
        <h2>Android</h2>
        {!guest ? <p>{online ? 'Reading Android…' : 'Start the emulator to see Android’s details.'}</p> : <KeyValues rows={[
          ['Device', /oculus/i.test(props['ro.product.manufacturer']) ? `${props['ro.product.manufacturer']} ${props['ro.product.model']}` : bad(`${props['ro.product.manufacturer']} ${props['ro.product.model']}`, 'no Quest identity yet; Play sets it')],
          ['Android', `${props['ro.build.version.release']} (API ${props['ro.build.version.sdk']})`],
          ['Build', props['ro.build.fingerprint'], 'mono'],
          ['ABIs', props['ro.product.cpu.abilist'], 'mono'],
          ['ARM translator', props['ro.dalvik.vm.native.bridge'] === 'libberberis_arm64.so' ? 'Digitalis (libberberis_arm64.so)' : bad(props['ro.dalvik.vm.native.bridge'] || 'none', 'ARM64 games will not start')],
          ['OpenGL ES', /swiftshader|llvmpipe|software/i.test(guest.gles) ? bad(guest.gles, 'software rendering') : guest.gles],
          ['Vulkan driver', props['ro.hardware.vulkan']],
          ['Clock source', guest.clocksource === 'tsc' ? 'tsc' : bad(guest.clocksource || 'unknown', 'not the TSC; games run slower')],
          ['CPUs', guest.cores],
          ['Memory', `${bytes(guest.memory.available)} free of ${bytes(guest.memory.total)}${guest.memory.swapTotal ? ` · swap ${bytes(guest.memory.swapTotal - guest.memory.swapFree)} used` : ''}`],
          ['Display', guest.display],
          ['SELinux', guest.selinux],
          ['Focused window', /Application Error|Not Responding|isn.t responding|keeps stopping/i.test(guest.focus) ? bad(guest.focus, 'this dialog keeps games black; Play closes it') : guest.focus, 'mono'],
          ['Full-screen notice', guest.immersiveConfirmed ? 'Dismissed' : bad('Not dismissed', 'Unity games can stay black; Play dismisses it')],
          ['Crash dialogs', guest.errorDialogsHidden ? 'Hidden' : bad('Shown', 'one can steal focus from a game; Play hides them')],
          ['Kernel', guest.kernel, 'mono'],
          ['Kernel command line', guest.cmdline, 'mono'],
        ]} />}
      </section>

      <section className="panel">
        <h2>Emulator process</h2>
        {!qemu ? <p>{status ? 'No emulator is running this virtual device.' : 'Checking…'}</p> : <KeyValues rows={[
          ['Process', `${qemu.name} (PID ${qemu.pid})`],
          ['qemu', qemu.multicore ? 'Multi-core copy' : bad('Stock', 'Android may get one vCPU')],
          ['vCPUs', qemu.cores === 1 ? bad('1', 'Settings > CPU cores, or the multi-core qemu is missing') : qemu.cores],
          ['Window', qemu.window ? 'Shown' : 'Hidden'],
          ['Started', qemu.started && `${new Date(qemu.started).toLocaleString()} (${ago(qemu.started)})`],
          ['Threads', qemu.threads],
          ['Executable', qemu.path, 'mono'],
          ['Command line', status.processes.find(p => p.pid === qemu.pid)?.command, 'mono small'],
        ]} />}
        {status?.processes.length > 0 && <table className="table">
          <thead><tr><th>Process</th><th>PID</th><th>CPU</th><th>Memory</th></tr></thead>
          <tbody>{status.processes.map(p => <tr key={p.pid}><td>{p.name}</td><td className="mono">{p.pid}</td>
            <td>{p.cpuPercent != null ? `${p.cpuPercent.toFixed(1)}%` : '…'}</td><td>{bytes(p.memory)}</td></tr>)}</tbody>
        </table>}
      </section>

      <section className="panel">
        <h2>Refract components</h2>
        {!info ? <p>Checking…</p> : <KeyValues rows={components} />}
        {Object.keys(props).some(k => /refract/.test(k)) && <>
          <h3>Refract properties</h3>
          <KeyValues rows={Object.entries(props).filter(([k]) => /refract/.test(k)).map(([k, v]) => [k, v || '(empty)', 'mono'])} />
        </>}
      </section>

      <section className="panel">
        <h2>This PC</h2>
        {!host ? <p>Checking…</p> : <KeyValues rows={[
          ['Windows', host.os],
          ['CPU', host.cpu && `${host.cpu} (${host.cores} cores, ${host.threads} threads)`],
          ['Memory', host.memory && `${bytes(host.free)} free of ${bytes(host.memory)}`],
          ['Graphics', host.gpus?.length ? <>{host.gpus.map(g => <div key={g}>{g}</div>)}</> : ''],
          ['OpenXR runtime', host.openxr || bad('None', 'VR play needs Meta Horizon Link or SteamVR'), 'mono'],
          ['Acceleration', host.acceleration && (host.acceleration.ok ? host.acceleration.detail : bad(host.acceleration.detail))],
          ['Android SDK', host.sdk, 'mono'],
          ['Emulator', `${host.emulatorVersion || 'not installed'}${host.multicoreQemu ? ' · multi-core qemu' : ' · no multi-core qemu'}`],
          ['adb', host.adbVersion],
          ['Node.js', host.node],
          ['Launcher', host.launcherVersion],
          ['Refract commit', host.commit, 'mono'],
          ['Refract folder', host.root, 'mono'],
          ['Launcher data', host.data, 'mono'],
        ]} />}
      </section>

      {guest && <section className="panel">
        <div className="panel-row"><h2>Installed apps</h2><span className="muted">{guest.packages.length}</span></div>
        <ul className="app-list">
          {guest.packages.map(p => <li key={p.package}>
            <div><span className="mono">{p.package}</span>{p.versionCode && <small> · code {p.versionCode}</small>}{game?.package === p.package && <span className="tag">Running</span>}</div>
            <button type="button" className="btn btn-ghost btn-sm" onClick={() => onLogcat(p.package)}><ScrollText />Logcat</button>
            <button type="button" className="btn btn-ghost btn-sm" disabled={pending.has('emu-forceStop')} onClick={() => action('forceStop', p.package, () => notify(`Stopped ${p.package}`))}><Square />Force stop</button>
          </li>)}
        </ul>
      </section>}

      {guest && <section className="panel">
        <h2>Busiest Android processes</h2>
        <pre className="pre">{guest.top}</pre>
        <h3>Storage</h3>
        <pre className="pre">{guest.storage}</pre>
      </section>}
    </div>
    {guest && <Properties props={props} />}
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
    {up && <p className="note">Changes to the emulator apply the next time it starts. Stop it at the top of this page, then start it again.</p>}
    <section className="panel">
      <h2>Virtual device</h2>
      <p>The Android 16 (API 36) virtual device Refract runs games in.</p>
      {field('sdk', 'Android SDK', { required: true }, true)}
      {field('avd', 'Virtual device', { required: true, pattern: '[a-zA-Z0-9_\\-]+' })}
      <div className="fields-2">
        {field('port', 'Port', { type: 'number', min: 5554, max: 5682, step: 2, required: true })}
        {field('memoryMB', 'Memory (MB)', { type: 'number', min: 2048, max: 16384, step: 1024, required: true })}
      </div>
    </section>
    <section className="panel">
      <h2>Performance</h2>
      <p>Each Android CPU keeps one PC thread busy. Refract uses at most half of this PC’s performance-core threads, so SteamVR and the host bridge keep the rest.</p>
      <div className="field">
        <label htmlFor="cores">CPU cores</label>
        <select id="cores" className="field-input" value={draft.cores} onChange={e => edit('cores', Number(e.target.value))}>
          {[1, 2, 3, 4, 5, 6].map(n => <option key={n} value={n}>{n}{n === 6 ? ' (default)' : n === 1 ? ' (stock emulator)' : ''}</option>)}
        </select>
      </div>
    </section>
    <section className="panel">
      <h2>Audio</h2>
      <p>How the emulator plays game sound on this PC.</p>
      <div className="field">
        <label htmlFor="audio">Audio output</label>
        <select id="audio" className="field-input" value={draft.audio} onChange={e => edit('audio', e.target.value)}>
          <option value="dsound">DirectSound (default, low latency)</option>
          <option value="winaudio">Windows audio (winaudio)</option>
          <option value="sdl">SDL</option>
        </select>
      </div>
      {toggle('hostMic', 'Microphone', 'Lets games hear this PC’s microphone (voice chat).')}
    </section>
    <section className="panel">
      <h2>Behavior</h2>
      {toggle('showWindow', 'Show the emulator window', 'Opens Android’s own window next to the game, for debugging. Games still show in the headset or the PC viewer.')}
      {toggle('keepEmulator', 'Keep Android running when Refract closes', 'Otherwise an emulator Refract started stops with it. Keeping it makes the next launch faster.')}
    </section>
    {saveBar}
  </form>;
}
