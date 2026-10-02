import { useState } from 'react';
import { call } from '../api';
import { activeStatuses } from './common';

const numeric = ['port', 'memoryMB', 'cores', 'pcEyeSize', 'vrRenderScale'];

// A draft of the launcher settings with field helpers and a save bar. Pages that use it are keyed
// by the saved settings, so a save elsewhere starts a fresh draft.
export function useSettingsForm({ state, run, pending, notify }) {
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
  const toggle = (key, label, detail) => <label className="toggle-row">
    <div><strong>{label}</strong>{detail && <span>{detail}</span>}</div>
    <input type="checkbox" className="switch" checked={Boolean(draft[key])} onChange={e => edit(key, e.target.checked)} />
  </label>;
  const save = e => {
    e.preventDefault();
    run('settings', async () => {
      await call('settings', { ...draft, ...Object.fromEntries(numeric.filter(k => k in draft).map(k => [k, Number(draft[k])])) });
      setDirty(false); notify('Settings saved');
    });
  };
  const saveBar = dirty && <div className="save-bar">
    <span>{locked ? 'Finish running games and downloads to save.' : 'You have unsaved changes.'}</span>
    <button type="button" className="btn btn-ghost" onClick={() => { setDraft({ ...state.settings }); setDirty(false); }}>Discard</button>
    <button type="submit" className="btn btn-primary" disabled={locked || pending.has('settings')}>Save changes</button>
  </div>;
  return { draft, edit, field, toggle, save, saveBar };
}
