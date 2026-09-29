import { useState } from 'react';
import { ChevronDown } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { activeStatuses, call, PageHeader } from './common';

function Section({ title, description, children }) {
  return <section className="rounded-2xl bg-card p-5 ring-1 ring-white/[0.05]">
    <h2 className="font-display text-[17px] font-normal">{title}</h2>
    {description && <p className="mt-1 text-[13px] text-muted-foreground">{description}</p>}
    <div className="mt-4 space-y-4">{children}</div>
  </section>;
}

export function Settings({ state, run, pending, notify }) {
  const [draft, setDraft] = useState({ ...state.settings });
  const [dirty, setDirty] = useState(false);
  const locked = state.busy || Boolean(state.running) || state.jobs.some(j => activeStatuses.includes(j.status));
  const edit = (key, value) => { setDraft(d => ({ ...d, [key]: value })); setDirty(true); };
  const browse = key => run(`choose-${key}`, async () => {
    const value = await call(key === 'ovrportCli' ? 'chooseCli' : 'chooseFolder');
    if (value) edit(key, value);
  });
  const field = (key, label, options = {}) => <div className="space-y-1.5">
    <label htmlFor={key} className="block text-[13px] text-foreground/85">{label}</label>
    <div className="flex gap-2"><Input id={key} name={key} value={draft[key] ?? ''} onChange={e => edit(key, e.target.value)} {...options} />
      {['downloadDir', 'ovrportCli'].includes(key) && <Button type="button" variant="outline" disabled={pending.has(`choose-${key}`)} onClick={() => browse(key)}>Browse</Button>}
    </div>
  </div>;
  return <>
    <PageHeader title="Settings" />
    <form id="settings-form" className="max-w-2xl space-y-4" onSubmit={e => {
      e.preventDefault(); run('settings', async () => {
        await call('settings', { ...draft, port: Number(draft.port), memoryMB: Number(draft.memoryMB) });
        setDirty(false); notify('Settings saved');
      });
    }}>
      <Section title="Meta account" description="Sign in on Meta's own page to list and download the Quest games you own.">
        <div className="flex items-center justify-between gap-4">
          <div className="flex min-w-0 items-center gap-3">
            <span className={state.signedIn ? 'size-2 rounded-full bg-white' : 'size-2 rounded-full border border-white/40'} />
            <span className="truncate">{state.signedIn ? state.account || 'Connected' : 'Not connected'}</span>
          </div>
          <Button type="button" variant={state.signedIn ? 'outline' : 'default'} disabled={pending.has('account')} onClick={() => run('account', () => call(state.signedIn ? 'logout' : 'login'))}>{state.signedIn ? 'Sign out' : 'Connect Meta'}</Button>
        </div>
      </Section>
      <Section title="Storage">{field('downloadDir', 'Download folder', { required: true })}</Section>
      <Section title="Patching" description="Optional. Used by “Patch with ovrport” in a game's menu. A .jar needs Java.">{field('ovrportCli', 'ovrport CLI', { placeholder: 'Choose an .exe or .jar' })}</Section>
      <details className="group rounded-2xl bg-card ring-1 ring-white/[0.05]" data-runtime-settings>
        <summary className="flex list-none items-center justify-between rounded-2xl p-5 [&::-webkit-details-marker]:hidden">
          <span><span className="block font-display text-[17px]">Android runtime</span><span className="mt-1 block text-[13px] text-muted-foreground">Emulator used to install and run games</span></span>
          <ChevronDown className="size-4 text-muted-foreground transition-transform group-open:rotate-180" />
        </summary>
        <div className="space-y-4 px-5 pb-5">{field('sdk', 'Android SDK', { required: true })}{field('avd', 'Virtual device', { required: true, pattern: '[a-zA-Z0-9_-]+' })}
          <div className="grid grid-cols-2 gap-4">{field('port', 'Port', { type: 'number', min: 5554, max: 5682, step: 2, required: true })}{field('memoryMB', 'Memory (MB)', { type: 'number', min: 2048, max: 16384, step: 1024, required: true })}</div>
        </div>
      </details>
      <div className="flex items-center gap-4 pt-2">
        <Button type="submit" disabled={!dirty || locked || pending.has('settings')}>Save changes</Button>
        <span className="text-[13px] text-muted-foreground">{locked ? 'Finish running tasks and games before changing settings.' : dirty ? 'Unsaved changes' : ''}</span>
      </div>
    </form>
  </>;
}
