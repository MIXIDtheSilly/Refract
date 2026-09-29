import { useState } from 'react';
import { call } from '../api';
import { activeStatuses } from '../components/common';

export function SettingsPage({ state, run, pending, notify, connect }) {
  const [draft, setDraft] = useState({ ...state.settings });
  const [dirty, setDirty] = useState(false);
  const edit = (key, value) => { setDraft(d => ({ ...d, [key]: value })); setDirty(true); };
  const browse = key => run(`choose-${key}`, async () => {
    const value = await call('chooseFolder');
    if (value) edit(key, value);
  });
  const locked = state.busy || Boolean(state.running) || state.jobs.some(j => activeStatuses.includes(j.status));
  const field = (key, label, options = {}, browseable = false) => <div className="field">
    <label htmlFor={key}>{label}</label>
    <div className="field-row">
      <input id={key} name={key} className="field-input" value={draft[key] ?? ''} onChange={e => edit(key, e.target.value)} spellCheck={false} {...options} />
      {browseable && <button type="button" className="btn btn-outline" disabled={pending.has(`choose-${key}`)} onClick={() => browse(key)}>Browse</button>}
    </div>
  </div>;
  const save = e => {
    e.preventDefault();
    run('settings', async () => {
      await call('settings', { ...draft, port: Number(draft.port), memoryMB: Number(draft.memoryMB) });
      setDirty(false); notify('Settings saved');
    });
  };

  return <div className="page">
    <header className="page-head"><div><h1 className="page-title">Settings</h1></div></header>
    <form id="settings-form" className="settings" onSubmit={save}>
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
        <h2>Android runtime</h2>
        <p>The emulator Refract runs games in. It must be an Android 16 (API 36) virtual device.</p>
        {field('sdk', 'Android SDK', { required: true })}
        {field('avd', 'Virtual device', { required: true, pattern: '[a-zA-Z0-9_\\-]+' })}
        <div className="fields-2">
          {field('port', 'Port', { type: 'number', min: 5554, max: 5682, step: 2, required: true })}
          {field('memoryMB', 'Memory (MB)', { type: 'number', min: 2048, max: 16384, step: 1024, required: true })}
        </div>
      </section>
      {dirty && <div className="save-bar">
        <span>{locked ? 'Finish running games and downloads to save.' : 'You have unsaved changes.'}</span>
        <button type="button" className="btn btn-ghost" onClick={() => { setDraft({ ...state.settings }); setDirty(false); }}>Discard</button>
        <button type="submit" className="btn btn-primary" disabled={locked || pending.has('settings')}>Save changes</button>
      </div>}
    </form>
  </div>;
}
