import { ExternalLink, LoaderCircle, Search } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { call, Empty, GameGrid, PageHeader } from './common';

export function Store({ query, setQuery, results, searched, onSearch, pending, run, onOpen, running }) {
  const searching = pending.has('search');
  return <>
    <PageHeader title="Store" subtitle="Search the Meta Quest catalog and add games to your library">
      <Button variant="outline" onClick={() => run('store', () => call('openStore'))}><ExternalLink />Meta store</Button>
    </PageHeader>
    <form id="store-search" onSubmit={onSearch} className="mb-7 flex items-center gap-2.5">
      <div className="relative w-full max-w-lg min-w-0"><Search className="pointer-events-none absolute top-3 left-4 size-4 text-muted-foreground" />
        <Input id="store-query" type="search" aria-label="Search Quest store" placeholder="Search Quest games" className="h-10 rounded-full pl-11 text-[15px]" value={query} onChange={e => setQuery(e.target.value)} required minLength={2} /></div>
      <Button size="lg" className="h-10" disabled={searching} type="submit">{searching ? <LoaderCircle className="animate-spin" aria-label="Searching" /> : 'Search'}</Button>
    </form>
    {results.length ? <><p className="mb-4 text-[13px] text-muted-foreground">{results.length} result{results.length === 1 ? '' : 's'}</p><GameGrid games={results} onOpen={onOpen} running={running} store /></>
      : searched && !searching ? <Empty hint="Check the spelling, or search by the game's full name.">No games found</Empty>
      : !searching && <Empty hint="Purchases open on Meta's site. Sign in to Meta to download the games you own.">Find Quest games</Empty>}
  </>;
}
