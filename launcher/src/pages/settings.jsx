import { Cpu } from 'lucide-react';
import { call } from '../api';
import { SetupPanel } from '../components/setup';
import { useSettingsForm } from '../components/settings-form';

export function SettingsPage({ state, run, pending, notify, connect, setPage }) {
  const { field, save, saveBar } = useSettingsForm({ state, run, pending, notify });

  return <div className="page">
    <header className="page-head"><div><h1 className="page-title">Settings</h1></div></header>
    <form id="settings-form" className="settings" onSubmit={save}>
      <SetupPanel state={state} run={run} pending={pending} notify={notify} />
      <section className="panel">
        <div className="panel-row">
          <div><h2>Meta account</h2><p style={{ margin: '3px 0 0', color: 'var(--muted)' }}>{state.signedIn ? `Connected${state.account ? ` as ${state.account}` : ''}. Your owned Quest games appear in the library.` : 'Sign in on Meta’s page to list and download the Quest games you own.'}</p></div>
          {state.signedIn
            ? <button type="button" className="btn btn-outline" disabled={pending.has('account')} onClick={() => run('account', () => call('logout'))}>Sign out</button>
            : <button type="button" className="btn btn-primary" disabled={pending.has('account')} onClick={connect}>Connect Meta</button>}
        </div>
      </section>
      <section className="panel">
        <h2>Downloads</h2>
        <p>Where APK and expansion files are saved. Five GB stays free so Android can still boot.</p>
        {field('downloadDir', 'Download folder', { required: true }, true)}
      </section>
      <section className="panel">
        <div className="panel-row">
          <div><h2>Android runtime</h2><p style={{ margin: '3px 0 0', color: 'var(--muted)' }}>The virtual device, memory, CPU cores and audio are on the Emulator page, with its logs and debug tools.</p></div>
          <button type="button" className="btn btn-outline" onClick={() => setPage('emulator')}><Cpu />Open Emulator</button>
        </div>
      </section>
      {saveBar}
    </form>
  </div>;
}
