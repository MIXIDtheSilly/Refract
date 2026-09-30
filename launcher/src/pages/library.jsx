import { useState } from 'react';
import { Plus, RefreshCw, Search } from 'lucide-react';
import { call } from '../api';
import { ago, Cover, Empty, GameStatus, IconButton } from '../components/common';
import { SetupBanner } from '../components/setup';
import { HeadsetStatus, PlayButtons } from '../components/play';

const filters = [['all', 'All'], ['installed', 'Installed'], ['downloaded', 'Downloaded']];

export function Library({ state, run, pending, open, notify, connect, setPage, headset }) {
  const [filter, setFilter] = useState('all');
  const [query, setQuery] = useState('');
  const games = state.games
    .filter(g => filter === 'all' || (filter === 'installed' ? g.installed : g.downloaded || g.apk))
    .filter(g => g.name.toLowerCase().includes(query.trim().toLowerCase()))
    .sort((a, b) => (b.lastPlayed || '').localeCompare(a.lastPlayed || '') || a.name.localeCompare(b.name));
  const recent = state.games.filter(g => g.lastPlayed && g.installed).sort((a, b) => b.lastPlayed.localeCompare(a.lastPlayed))[0];
  const installed = state.games.filter(g => g.installed).length;
  const refresh = () => run('sync', async () => {
    const info = await call('sync');
    if (!info.online) notify('Android isn’t running, so installed games weren’t rescanned. It starts when you install or play a game.');
    else if (info.meta?.partial) notify('Meta returned only part of your library.');
    else notify('Library refreshed');
  });

  return <div className="page">
    <header className="page-head">
      <div>
        <h1 className="page-title">Library</h1>
        <div className="page-sub">{state.games.length} {state.games.length === 1 ? 'game' : 'games'} · {installed} installed</div>
      </div>
      <HeadsetStatus {...headset} />
    </header>
    <SetupBanner run={run} setPage={setPage} />

    {recent && !query && filter === 'all' && <section className="hero" aria-label="Continue playing">
      <Cover game={recent} />
      <div className="hero-body">
        <div className="hero-kicker">Continue playing</div>
        <h2 className="hero-title">{recent.name}</h2>
        <div className="hero-meta">Last played {ago(recent.lastPlayed)}</div>
        <div className="hero-actions">
          <PlayButtons game={recent} state={state} run={run} pending={pending} notify={notify} headset={headset.headset} />
          <button type="button" className="btn btn-ghost btn-lg" onClick={() => open(recent.id)}>Details</button>
        </div>
      </div>
    </section>}

    <div className="toolbar">
      <div className="search"><Search />
        <input id="library-search" className="field-input" type="search" placeholder="Search library" aria-label="Search library" value={query} onChange={e => setQuery(e.target.value)} />
      </div>
      <div className="segmented" role="group" aria-label="Filter library">
        {filters.map(([id, label]) => <button key={id} type="button" aria-pressed={filter === id} onClick={() => setFilter(id)}>{label}</button>)}
      </div>
      <div className="grow" />
      <IconButton label="Refresh library" disabled={pending.has('sync')} onClick={refresh}><RefreshCw className={pending.has('sync') ? 'spin' : ''} /></IconButton>
      <button type="button" className="btn btn-primary" disabled={state.busy || pending.has('import')} onClick={() => run('import', () => call('import'))}><Plus />Import APK</button>
    </div>

    {games.length
      ? <div className="grid">{games.map(game => <button key={game.id} type="button" className="card" data-game={game.id} onClick={() => open(game.id)} aria-label={`Open ${game.name}`}>
        <Cover game={game} />
        <div className="card-name" title={game.name}>{game.name}</div>
        <div className="card-meta"><GameStatus game={game} running={state.running === game.id} /></div>
      </button>)}</div>
      : state.games.length
        ? <Empty title="No matching games">Try another search or filter.</Empty>
        : <Empty title="Your library is empty" action={<div className="hero-actions">
          {!state.signedIn && <button type="button" className="btn btn-primary" disabled={pending.has('account')} onClick={connect}>Connect Meta</button>}
          <button type="button" className={`btn ${state.signedIn ? 'btn-primary' : 'btn-outline'}`} onClick={() => run('import', () => call('import'))}>Import APK</button>
        </div>}>Connect Meta to see the Quest games you own, or import an APK.</Empty>}
  </div>;
}
