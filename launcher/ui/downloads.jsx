import { Check, CircleAlert, LoaderCircle } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { cn } from '@/lib/utils';
import { activeStatuses, bytes, call, Cover, Empty, PageHeader } from './common';

const labels = { queued: 'Queued', downloading: 'Downloading', installing: 'Installing', patching: 'Patching', complete: 'Complete', failed: 'Failed', interrupted: 'Interrupted', cancelled: 'Cancelled' };
function Status({ status }) {
  const failed = ['failed', 'interrupted'].includes(status);
  return <span className={cn('flex shrink-0 items-center gap-1.5 text-xs', failed ? 'text-destructive' : status === 'complete' ? 'text-foreground/80' : 'text-muted-foreground')}>
    {activeStatuses.includes(status) && status !== 'queued' ? <LoaderCircle className="size-3.5 animate-spin" /> : status === 'complete' ? <Check className="size-3.5" /> : failed ? <CircleAlert className="size-3.5" /> : null}
    {labels[status] || status}
  </span>;
}

export function Downloads({ jobs, games, run, pending }) {
  const active = jobs.filter(j => activeStatuses.includes(j.status)).length;
  return <>
    <PageHeader title="Downloads" subtitle={active ? `${active} active` : jobs.length ? 'Nothing in progress' : null} />
    <div className="jobs max-w-3xl space-y-2.5">{jobs.length ? jobs.map(job => {
      const game = games.find(g => g.id === job.gameId) || { name: job.name };
      const percent = job.total ? Math.min(100, Math.round((job.completed || 0) / job.total * 100)) : null;
      return <article key={job.id} className="fade-up flex gap-4 rounded-xl bg-card p-3.5 ring-1 ring-white/[0.05]">
        <Cover game={game} className="aspect-[4/3] w-[72px] shrink-0 rounded-lg [&_span]:text-lg" />
        <div className="min-w-0 flex-1 self-center">
          <div className="flex items-center justify-between gap-4"><span className="truncate font-medium">{job.name}</span><Status status={job.status} /></div>
          <div className="mt-1 flex justify-between gap-4 text-xs text-muted-foreground"><span className="truncate">{job.stage}</span>
            {Boolean(job.total) && <span className="shrink-0 tabular-nums">{bytes(job.completed)} / {bytes(job.total)}{job.status === 'downloading' && percent !== null && ` · ${percent}%`}</span>}</div>
          {job.status === 'downloading' && <progress className="mt-2.5" value={job.total ? job.completed || 0 : undefined} max={job.total || undefined} aria-label={`${job.name} download progress`} />}
          {job.error && <p className="mt-2 text-xs break-words text-destructive">{job.error}</p>}
        </div>
        {['queued', 'downloading'].includes(job.status) ? <Button className="self-center" variant="outline" size="sm" disabled={pending.has(job.id)} onClick={() => run(job.id, () => call('cancel', job.id))}>Cancel</Button>
          : ['failed', 'interrupted', 'cancelled'].includes(job.status) && /^\d+$/.test(job.gameId) ? <Button className="self-center" size="sm" disabled={pending.has(job.id)} onClick={() => run(job.id, () => call('retry', job.id))}>Retry</Button> : null}
      </article>;
    }) : <Empty hint="Downloads and installs from your library show up here.">No downloads</Empty>}</div>
  </>;
}
