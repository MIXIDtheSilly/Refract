import { useCallback, useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { CircleAlert, Download, LayoutGrid, LoaderCircle, Settings as SettingsIcon, Square, Store as StoreIcon, X } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { cn } from '@/lib/utils';
import { activeStatuses, call, Cover, Empty } from './common';
import { Wordmark } from './logo';
import { Library } from './library';
import { Store } from './store';
import { Settings } from './settings';
import { Downloads } from './downloads';
import { GameDetails } from './game-details';
import './style.css';

function NavItem({ id, label, icon: Icon, page, setPage, badge }) {
  const current = page === id;
  return <button data-nav={id} aria-current={current ? 'page' : undefined} onClick={() => setPage(id)}
    className={cn('flex h-10 w-full items-center gap-3 rounded-full px-4 text-[14px] text-foreground/70 transition-colors hover:bg-white/[0.06] hover:text-foreground',
      current && 'bg-white/[0.1] font-medium text-foreground hover:bg-white/[0.1]')}>
    <Icon className="size-[18px]" strokeWidth={current ? 2.1 : 1.8} />{label}
    {badge > 0 && <span className="ml-auto flex h-5 min-w-5 items-center justify-center rounded-full bg-white px-1.5 text-[11px] font-semibold text-[#1c1c1c]">{badge}</span>}
  </button>;
}

function Sidebar({ state, page, setPage, run, pending, notify, onOpen }) {
  const activeDownloads = state?.jobs.filter(j => activeStatuses.includes(j.status)).length || 0;
  const playing = state?.games.find(g => g.id === state.running);
  return <aside className="flex w-60 shrink-0 flex-col gap-1 bg-sidebar px-3 pt-2 pb-3">
    <nav aria-label="Main navigation" className="space-y-1">
      <NavItem id="library" label="Library" icon={LayoutGrid} page={page} setPage={setPage} />
      <NavItem id="store" label="Store" icon={StoreIcon} page={page} setPage={setPage} />
      <NavItem id="downloads" label="Downloads" icon={Download} page={page} setPage={setPage} badge={activeDownloads} />
    </nav>
    <div className="flex-1" />
    {playing && <div className="fade-up mb-2 rounded-2xl bg-white/[0.05] p-3 ring-1 ring-white/[0.06]">
      <button className="flex w-full min-w-0 items-center gap-3 text-left" onClick={() => onOpen(playing.id)}>
        <Cover game={playing} className="w-12 shrink-0 rounded-lg [&_span]:text-sm" />
        <span className="min-w-0"><span className="flex items-center gap-1.5 text-[11px] font-medium tracking-[0.12em] text-white/55 uppercase"><span className="size-1.5 animate-pulse rounded-full bg-white" />Playing</span>
          <span className="mt-0.5 block truncate font-medium">{playing.name}</span></span>
      </button>
      <Button size="sm" variant="outline" className="mt-3 w-full" disabled={pending.has(`game-${playing.id}`)} onClick={() => run(`game-${playing.id}`, async () => { await call('stop'); notify('Closing game'); })}><Square className="size-3 fill-current" />Stop game</Button>
    </div>}
    {state && (state.signedIn
      ? <div className="mb-1 flex items-center gap-3 px-3 py-2"><span className="flex size-8 shrink-0 items-center justify-center rounded-full bg-white/[0.1] font-display text-[13px]">{(state.account || 'M')[0].toUpperCase()}</span>
          <span className="min-w-0"><span className="block truncate text-[13px]">{state.account || 'Meta account'}</span><span className="block text-xs text-muted-foreground">Meta connected</span></span></div>
      : <Button className="mb-2 w-full" disabled={pending.has('account')} onClick={() => run('account', () => call('login'))}>{pending.has('account') ? <><LoaderCircle className="animate-spin" />Connecting…</> : 'Connect Meta'}</Button>)}
    <NavItem id="settings" label="Settings" icon={SettingsIcon} page={page} setPage={setPage} />
  </aside>;
}

function App() {
  const [state, setState] = useState(null);
  const [page, setPage] = useState('library');
  const [filter, setFilter] = useState('all');
  const [libraryQuery, setLibraryQuery] = useState('');
  const [query, setQuery] = useState('');
  const [results, setResults] = useState([]);
  const [searched, setSearched] = useState(false);
  const [selected, setSelected] = useState(null);
  const [pending, setPending] = useState(new Set());
  const pendingRef = useRef(new Set());
  const [notice, setNotice] = useState(null);
  const timer = useRef(null);
  const content = useRef(null);
  const notify = useCallback((text, error = false) => {
    clearTimeout(timer.current); setNotice({ text, error });
    if (!error) timer.current = setTimeout(() => setNotice(null), 4000);
  }, []);
  const run = useCallback(async (key, task) => {
    if (pendingRef.current.has(key)) return;
    pendingRef.current.add(key); setPending(new Set(pendingRef.current));
    try { return await task(); } catch (error) { notify(error.message, true); }
    finally { pendingRef.current.delete(key); setPending(new Set(pendingRef.current)); }
  }, [notify]);
  useEffect(() => {
    let active = true;
    const off = window.refract.onChange(next => { if (active) setState(next); });
    const offError = window.refract.onLaunchError(error => notify(error, true));
    call('state').then(value => { if (active) setState(value); }).catch(error => notify(error.message, true));
    return () => { active = false; off(); offError(); clearTimeout(timer.current); };
  }, [notify]);
  useEffect(() => { content.current?.scrollTo(0, 0); }, [page]);
  const refresh = () => run('sync', async () => {
    const info = await call('sync');
    if (info.meta?.partial) notify('Meta returned a partial library. Add other games from the store.');
  });
  const search = e => {
    e.preventDefault(); const text = query.trim(); if (text.length < 2) return;
    run('search', async () => { const games = await call('search', text); setResults(games); setSearched(true); });
  };
  const game = state?.games.find(g => g.id === selected) || results.find(g => g.id === selected);
  return <div className="flex h-full flex-col">
    <div className="drag flex h-[var(--titlebar-height)] shrink-0 items-center bg-sidebar pr-[150px] pl-5">
      <button onClick={() => setPage('library')} aria-label="Refract library" className="no-drag rounded-md text-[17px] text-foreground"><Wordmark /></button>
      {state?.busy && <span className="ml-5 flex items-center gap-2 text-xs text-muted-foreground"><LoaderCircle className="size-3.5 animate-spin" />Working…</span>}
    </div>
    <div className="flex min-h-0 flex-1">
      <Sidebar state={state} page={page} setPage={setPage} run={run} pending={pending} notify={notify} onOpen={setSelected} />
      <main ref={content} id="content" aria-label={page} className="min-w-0 flex-1 overflow-y-auto rounded-tl-2xl bg-background">
        <div className="mx-auto max-w-[1480px] px-9 pt-8 pb-12">
          {!state ? <Empty action={<Button variant="outline" onClick={() => run('state', async () => setState(await call('state')))}>Load library</Button>}>Loading library…</Empty> : <>
            {page === 'library' && <Library state={state} run={run} pending={pending} notify={notify} filter={filter} setFilter={setFilter} query={libraryQuery} setQuery={setLibraryQuery} onOpen={setSelected} refresh={refresh} />}
            {page === 'store' && <Store query={query} setQuery={setQuery} results={results} searched={searched} onSearch={search} pending={pending} run={run} onOpen={setSelected} running={state.running} />}
            {page === 'downloads' && <Downloads jobs={state.jobs} games={state.games} run={run} pending={pending} />}
            {page === 'settings' && <Settings state={state} run={run} pending={pending} notify={notify} />}
          </>}
        </div>
      </main>
    </div>
    {notice && <div role={notice.error ? 'alert' : 'status'} className="fade-up fixed right-6 bottom-6 z-[100] flex max-w-md items-start gap-3 rounded-xl border border-white/10 bg-[#3a3a3a] py-3 pr-3 pl-4 shadow-[0_12px_32px_rgb(0_0_0/0.45)]">
      {notice.error && <CircleAlert className="mt-0.5 size-4 shrink-0 text-destructive" />}
      <span className="min-w-0 break-words">{notice.text}</span>
      <button aria-label="Dismiss notification" onClick={() => setNotice(null)} className="shrink-0 rounded-full p-0.5 text-muted-foreground hover:text-foreground"><X className="size-4" /></button>
    </div>}
    {game && state && <GameDetails key={game.id} game={game} state={state} local={state.games.some(g => g.id === game.id)} onClose={() => setSelected(null)} run={run} pending={pending} setPage={setPage} notify={notify} />}
  </div>;
}

createRoot(document.getElementById('root')).render(<App />);
