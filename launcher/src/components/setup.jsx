import { useCallback, useEffect, useState } from 'react';
import { AlertCircle, CheckCircle2, Loader2, RefreshCw, Wrench } from 'lucide-react';
import { call } from '../api';

// Runs the backend's setup checks. Checks take a few seconds (the emulator's acceleration check).
export function useSetup(run) {
  const [checks, setChecks] = useState(null);
  const refresh = useCallback(() => run('setup', async () => setChecks(await call('setup'))), [run]);
  useEffect(() => { refresh(); }, [refresh]);
  return [checks, refresh, setChecks];
}

export function SetupBanner({ run, setPage }) {
  const [checks] = useSetup(run);
  const missing = checks?.filter(c => !c.ok && !c.optional) || [];
  if (!missing.length) return null;
  return <section className="setup-banner" role="status">
    <AlertCircle />
    <div><strong>Refract needs {missing.length === 1 ? 'one more thing' : `${missing.length} more things`} before games can run</strong>
      <span>{missing.map(c => c.title).join(' · ')}</span></div>
    <button type="button" className="btn btn-primary btn-sm" onClick={() => setPage('settings')}>Finish setup</button>
  </section>;
}

export function SetupPanel({ state, run, pending, notify }) {
  const [checks, refresh, setChecks] = useSetup(run);
  const fix = check => run(`fix-${check.id}`, async () => {
    setChecks(await call('fixSetup', check.fix.action));
    notify(check.fix.action === 'hypervisor' ? 'Restart Windows to finish turning on Windows Hypervisor Platform.'
      : check.fix.action === 'android-studio' ? 'Open Android Studio once so it downloads the Android SDK, then check again.' : `${check.title} is set up`);
  });
  const ready = checks && checks.every(c => c.ok || c.optional);
  return <section className="panel" aria-labelledby="setup-title">
    <div className="panel-row">
      <div><h2 id="setup-title">Setup</h2>
        <p style={{ margin: '3px 0 0', color: 'var(--muted)' }}>{!checks ? 'Checking this PC…' : ready ? 'Everything Refract needs is ready.' : 'Fix the items below, then install or play a game.'}</p></div>
      <button type="button" className="btn btn-outline btn-sm" disabled={pending.has('setup') || Boolean(state.fixing)} onClick={refresh}>
        {pending.has('setup') ? <Loader2 className="spin" /> : <RefreshCw />}Check again</button>
    </div>
    {checks && <ul className="setup-list">
      {checks.map(check => <li key={check.id} className={check.ok ? 'ok' : 'missing'}>
        {check.ok ? <CheckCircle2 aria-label="Ready" /> : <AlertCircle aria-label="Needs attention" />}
        <div className="setup-text"><strong>{check.title}</strong><span>{check.detail}</span>
          {!check.ok && check.fix?.note && <small>{check.fix.note}</small>}</div>
        {!check.ok && check.fix && <button type="button" className="btn btn-primary btn-sm" disabled={Boolean(state.fixing)} onClick={() => fix(check)}>
          {state.fixing === check.fix.action ? <><Loader2 className="spin" />Working…</> : <><Wrench />{check.fix.label}</>}</button>}
      </li>)}
    </ul>}
  </section>;
}
