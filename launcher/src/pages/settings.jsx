import { Cpu, RefreshCw } from 'lucide-react';
import { call } from '../api';
import { SetupPanel } from '../components/setup';
import { useSettingsForm } from '../components/settings-form';

// A random id in the range the backend picks from (core/runtime.mjs newUserId): 16-17 digits, below 2^53.
function newUserId() {
  const [high, low] = crypto.getRandomValues(new Uint32Array(2));
  const start = 10n ** 15n, span = 2n ** 53n - start;
  return String(start + ((BigInt(high) << 32n) | BigInt(low)) % span);
}

export function SettingsPage({ state, run, pending, notify, connect, setPage }) {
  const { draft, edit, field, save, saveBar } = useSettingsForm({ state, run, pending, notify });

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
        <h2>Player</h2>
        <p>The Meta user ID games see. Online games use it to tell players apart, so each install gets a random one. Change it to play as someone else, or enter your old ID to get your game progress back. Applies the next time a game starts.</p>
        <div className="field">
          <label htmlFor="userId">User ID</label>
          <div className="field-row">
            <input id="userId" name="userId" className="field-input" value={draft.userId ?? ''} onChange={e => edit('userId', e.target.value.trim())}
              inputMode="numeric" pattern="[1-9][0-9]{0,18}" title="A whole number, up to 19 digits, not starting with 0" required spellCheck={false} />
            <button type="button" className="btn btn-outline" onClick={() => edit('userId', newUserId())}><RefreshCw />New ID</button>
          </div>
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
