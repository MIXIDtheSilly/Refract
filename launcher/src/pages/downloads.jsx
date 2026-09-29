import { call } from '../api';
import { activeStatuses, bytes, Cover, Empty } from '../components/common';

const labels = { queued: 'Queued', downloading: 'Downloading', installing: 'Installing', complete: 'Done', failed: 'Failed', cancelled: 'Cancelled', interrupted: 'Interrupted' };

export function Downloads({ state, run, pending, setPage }) {
  const games = new Map(state.games.map(g => [g.id, g]));
  const active = state.jobs.filter(j => activeStatuses.includes(j.status));
  const done = state.jobs.filter(j => !activeStatuses.includes(j.status));
  const job = item => {
    const game = games.get(item.gameId) || { id: item.gameId, name: item.name };
    const percent = item.total ? Math.min(100, (item.completed || 0) / item.total * 100) : 0;
    return <article key={item.id} className="job" aria-label={item.name}>
      <Cover game={game} />
      <div style={{ minWidth: 0 }}>
        <div className="job-name">{item.name}</div>
        <div className="job-line">
          <span>{item.stage}</span>
          <span className="status">{item.status === 'downloading' && item.total
            ? <span style={{ fontVariantNumeric: 'tabular-nums' }}>{bytes(item.completed)} / {bytes(item.total)}</span>
            : labels[item.status] || item.status}</span>
        </div>
        {activeStatuses.includes(item.status) && <div className={`progress ${item.status === 'downloading' && item.total ? '' : 'indeterminate'}`}
          role="progressbar" aria-label={`${item.name} progress`} aria-valuenow={item.total ? Math.round(percent) : undefined} aria-valuemin={0} aria-valuemax={100}>
          <div style={item.total ? { width: `${percent}%` } : undefined} />
        </div>}
        {item.error && <div className="job-error">{item.error}</div>}
      </div>
      <div>
        {['queued', 'downloading'].includes(item.status)
          ? <button type="button" className="btn btn-outline btn-sm" disabled={pending.has(item.id)} onClick={() => run(item.id, () => call('cancel', item.id))}>Cancel</button>
          : ['failed', 'interrupted', 'cancelled'].includes(item.status) && /^\d+$/.test(item.gameId)
            ? <button type="button" className="btn btn-primary btn-sm" disabled={pending.has(item.id)} onClick={() => run(item.id, () => call('retry', item.id))}>Retry</button>
            : null}
      </div>
    </article>;
  };
  return <div className="page">
    <header className="page-head"><div>
      <h1 className="page-title">Downloads</h1>
      <div className="page-sub">{active.length ? `${active.length} in progress` : 'Nothing in progress'}</div>
    </div></header>
    {state.jobs.length ? <>
      {active.length > 0 && <><h2 className="section-label">In progress</h2><div className="jobs" style={{ marginBottom: 34 }}>{active.map(job)}</div></>}
      {done.length > 0 && <><h2 className="section-label">Recent</h2><div className="jobs">{done.map(job)}</div></>}
    </> : <Empty title="No downloads yet" action={<button type="button" className="btn btn-primary" onClick={() => setPage('library')}>Go to library</button>}>
      Downloads and installs you start from a game’s page show up here.
    </Empty>}
  </div>;
}
