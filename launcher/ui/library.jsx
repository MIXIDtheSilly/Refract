import { Play, Plus, RefreshCw, Search, Square } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { ago, call, Empty, GameGrid, IconButton, PageHeader, safeImage } from './common';

function ContinuePlaying({ game, state, run, pending, notify, onOpen }) {
  const image = safeImage(game.image);
  const running = state.running === game.id;
  const busy = state.busy || pending.has(`game-${game.id}`);
  return <section aria-label="Continue playing" className="relative mb-9 overflow-hidden rounded-2xl bg-card ring-1 ring-white/[0.06]">
    {image && <img src={image} alt="" className="absolute inset-0 h-full w-full scale-110 object-cover opacity-50 blur-2xl" />}
    <div className="absolute inset-0 bg-gradient-to-r from-[#262626] via-[#262626]/85 to-[#262626]/30" />
    <div className="relative flex items-center gap-7 p-6">
      <div className="min-w-0 flex-1">
        <div className="text-xs font-medium tracking-[0.14em] text-white/55 uppercase">{running ? 'Now playing' : 'Continue playing'}</div>
        <div className="mt-2 truncate font-display text-[34px] leading-tight font-normal">{game.name}</div>
        <div className="mt-1 text-[13px] text-muted-foreground">{running ? 'Running in the Android runtime' : `Last played ${ago(game.lastPlayed)}`}</div>
        <div className="mt-5 flex items-center gap-2">
          {running ? <Button size="lg" disabled={busy} onClick={() => run(`game-${game.id}`, async () => { await call('stop'); notify('Closing game'); })}><Square className="fill-current" />Stop</Button>
            : <Button size="lg" disabled={busy || Boolean(state.running)} onClick={() => run(`game-${game.id}`, () => call('play', game.id))}><Play className="fill-current" />Play</Button>}
          <Button size="lg" variant="outline" onClick={() => onOpen(game.id)}>Details</Button>
        </div>
      </div>
      {image && <img src={image} alt="" className="hidden aspect-video w-72 shrink-0 rounded-xl object-cover shadow-[0_8px_30px_rgb(0_0_0/0.4)] ring-1 ring-white/10 min-[1100px]:block" />}
    </div>
  </section>;
}

export function Library({ state, run, pending, notify, filter, setFilter, query, setQuery, onOpen, refresh }) {
  const games = state.games.filter(g => (filter === 'all' || Boolean(g[filter])) && g.name.toLowerCase().includes(query.toLowerCase()));
  const installed = state.games.filter(g => g.installed).length;
  const recent = !query && filter === 'all' && (state.games.find(g => g.id === state.running)
    || state.games.filter(g => g.installed && g.lastPlayed).sort((a, b) => b.lastPlayed.localeCompare(a.lastPlayed))[0]);
  return <>
    <PageHeader title="Library" subtitle={state.games.length ? `${state.games.length} game${state.games.length === 1 ? '' : 's'} · ${installed} installed` : 'Your Quest games'}>
      <Button disabled={state.busy || pending.has('import')} onClick={() => run('import', () => call('import'))}><Plus />Import APK</Button>
    </PageHeader>
    {recent && <ContinuePlaying game={recent} state={state} run={run} pending={pending} notify={notify} onOpen={onOpen} />}
    <div className="mb-6 flex items-center gap-2.5">
      <div className="relative w-64 min-w-0"><Search className="pointer-events-none absolute top-2.5 left-3.5 size-4 text-muted-foreground" />
        <Input id="library-search" type="search" placeholder="Search library" aria-label="Search library" className="rounded-full pl-10" value={query} onChange={e => setQuery(e.target.value)} /></div>
      <Select value={filter} onValueChange={setFilter}><SelectTrigger aria-label="Filter library" className="w-36"><SelectValue /></SelectTrigger>
        <SelectContent><SelectItem value="all">All games</SelectItem><SelectItem value="installed">Installed</SelectItem><SelectItem value="downloaded">Downloaded</SelectItem></SelectContent></Select>
      <div className="flex-1" />
      <IconButton label="Refresh library" disabled={pending.has('sync')} onClick={refresh}><RefreshCw className={pending.has('sync') ? 'animate-spin' : ''} /></IconButton>
    </div>
    {games.length ? <GameGrid games={games} onOpen={onOpen} running={state.running} />
      : state.games.length ? <Empty hint="Try a different search or filter.">No matching games</Empty>
      : <Empty hint="Refresh to import games already installed in Android, find games in the store, or import an APK you own."
          action={<Button variant="outline" disabled={pending.has('sync')} onClick={refresh}><RefreshCw className={pending.has('sync') ? 'animate-spin' : ''} />Refresh</Button>}>Your library is empty</Empty>}
  </>;
}
