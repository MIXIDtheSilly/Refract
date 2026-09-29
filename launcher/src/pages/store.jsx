import { useState } from 'react';
import { ExternalLink, Loader2, Search } from 'lucide-react';
import { call } from '../api';
import { Cover, Empty, isFree } from '../components/common';
import { LogoMark } from '../components/logo';

const suggestions = ['Rhythm', 'Puzzle', 'Multiplayer', 'Horror', 'Fitness', 'Racing'];

export function Store({ state, run, pending, open, results, setResults }) {
  const [query, setQuery] = useState(results.query || '');
  const search = text => {
    const value = text.trim(); if (value.length < 2) return;
    setQuery(value);
    run('search', async () => setResults({ query: value, games: await call('search', value) }));
  };
  const owned = new Set(state.games.map(g => g.id));
  return <div className="page">
    <div className="store-hero">
      <LogoMark className="mark" />
      <h1>Quest Store</h1>
      <p>Search the Meta Quest catalog and add games to your library.</p>
      <form id="store-search" className="store-search" onSubmit={e => { e.preventDefault(); search(query); }}>
        <div className="search"><Search />
          <input id="store-query" className="field-input" type="search" aria-label="Search Quest store" placeholder="Search Quest games"
            value={query} onChange={e => setQuery(e.target.value)} required minLength={2} />
        </div>
        <button type="submit" className="btn btn-primary btn-lg" disabled={pending.has('search')}>{pending.has('search') ? <Loader2 className="spin" aria-label="Searching" /> : 'Search'}</button>
      </form>
      {!results.games && <div className="chips">
        {suggestions.map(s => <button key={s} type="button" className="chip" onClick={() => search(s)}>{s}</button>)}
        <button type="button" className="chip" onClick={() => run('store', () => call('openStore'))}>Open meta.com <ExternalLink style={{ width: 12, verticalAlign: -1 }} /></button>
      </div>}
    </div>
    {results.games && (results.games.length
      ? <>
        <h2 className="section-label">{results.games.length} results for “{results.query}”</h2>
        <div className="grid">{results.games.map(game => <button key={game.id} type="button" className="card" data-game={game.id} data-store onClick={() => open(game.id)} aria-label={`Open ${game.name}`}>
          <Cover game={game} badge={owned.has(game.id) ? 'In library' : undefined} />
          <div className="card-name" title={game.name}>{game.name}</div>
          <div className="card-meta">{isFree(game.price) ? 'Free' : game.price}{game.publisher && <> · {game.publisher}</>}</div>
        </button>)}</div>
      </>
      : !pending.has('search') && <Empty title="No games found">Nothing matched “{results.query}”. Try a different name, or paste a store link.</Empty>)}
  </div>;
}
