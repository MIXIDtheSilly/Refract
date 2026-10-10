import { Cpu, RefreshCw } from 'lucide-react';
import { call } from '../api';
import { SetupPanel } from '../components/setup';
import { useSettingsForm } from '../components/settings-form';
import { RuntimeChoice } from '../components/runtime-choice';

// A random id in the range the backend picks from (core/runtime.mjs newUserId): 16-17 digits, below 2^53.
function newUserId() {
  const [high, low] = crypto.getRandomValues(new Uint32Array(2));
  const start = 10n ** 15n, span = 2n ** 53n - start;
  return String(start + ((BigInt(high) << 32n) | BigInt(low)) % span);
}

// Render resolution presets (backend: validEyeSize / validRenderScale in core/runtime.mjs).
const eyeSizes = [1024, 1280, 1440, 1600, 1920, 2048, 2560, 3072];
const renderScales = [50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200];
const withCurrent = (list, value) => Number.isInteger(value) && !list.includes(value) ? [...list, value].sort((a, b) => a - b) : list;

export function SettingsPage({ state, run, pending, notify, connect, setPage }) {
  const { draft, edit, field, save, saveBar } = useSettingsForm({ state, run, pending, notify });

  return <div className="page">
    <header className="page-head"><div><h1 className="page-title">Settings</h1></div></header>
    <form id="settings-form" className="settings" onSubmit={save}>
      <section className="panel">
        <h2>Runtime</h2>
        <p>How Refract runs games. Games are installed separately for each runtime, so switching shows the games installed for that one.</p>
        <RuntimeChoice value={draft.backend} onChange={value => edit('backend', value)} />
      </section>
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
        <h2>Display</h2>
        <p>The resolution games render at. Lower is faster; higher is sharper. Applies the next time a game starts.</p>
        <div className="fields-2">
          <div className="field">
            <label htmlFor="pcEyeSize">PC window (per eye)</label>
            <select id="pcEyeSize" className="field-input" value={draft.pcEyeSize} onChange={e => edit('pcEyeSize', Number(e.target.value))}>
              {withCurrent(eyeSizes, draft.pcEyeSize).map(n => <option key={n} value={n}>{n} × {n}{n === 1600 ? ' (default)' : ''}</option>)}
            </select>
          </div>
          <div className="field">
            <label htmlFor="vrRenderScale">VR headset (render scale)</label>
            <select id="vrRenderScale" className="field-input" value={draft.vrRenderScale} onChange={e => edit('vrRenderScale', Number(e.target.value))}
              title="Percent of the eye size SteamVR or Meta Link recommends for your headset">
              {withCurrent(renderScales, draft.vrRenderScale).map(n => <option key={n} value={n}>{n}%{n === 100 ? ' (headset default)' : ''}</option>)}
            </select>
          </div>
        </div>
      </section>
      <section className="panel">
        <h2>Downloads</h2>
        <p>Where APK and expansion files are saved. Five GB stays free so Android can still boot.</p>
        {field('downloadDir', 'Download folder', { required: true }, true)}
      </section>
      {draft.backend !== 'native' && <section className="panel">
        <div className="panel-row">
          <div><h2>Android runtime</h2><p style={{ margin: '3px 0 0', color: 'var(--muted)' }}>The virtual device, memory, CPU cores and audio are on the Emulator page, with its logs and debug tools.</p></div>
          <button type="button" className="btn btn-outline" onClick={() => setPage('emulator')}><Cpu />Open Emulator</button>
        </div>
      </section>}
      {saveBar}
    </form>
  </div>;
}
