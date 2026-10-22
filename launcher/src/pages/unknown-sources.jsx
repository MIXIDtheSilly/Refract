import { useCallback, useEffect, useState } from 'react';
import { ArrowDownToLine, FilePlus2, FolderPlus, RefreshCcw, Trash2, TriangleAlert } from 'lucide-react';
import { call } from '../api';
import { Cover, Empty, GameStatus, IconButton } from '../components/common';
import '../unknown-sources.css';

// Games that did not come from the Meta store: your own builds, itch.io releases, ports. The backend keeps them apart from
// Meta's library (backend/sideload.mjs); installing and playing them is the normal game flow, opened from Details.
export function UnknownSources({ state, run, pending, notify, open }) {
  const games = state.games.filter(g => g.unknownSource).sort((a, b) => a.name.localeCompare(b.name));
  const [disk, setDisk] = useState({});
  const [result, setResult] = useState(null);
  const ids = games.map(g => g.id).join();

  const refreshDisk = useCallback(() => call('sideloadStatus').then(setDisk, () => {}), []);
  // The APK is rebuilt outside Refract, so look again when the page opens, the window gets focus and the list changes.
  useEffect(() => {
    refreshDisk();
    window.addEventListener('focus', refreshDisk);
    return () => window.removeEventListener('focus', refreshDisk);
  }, [refreshDisk, ids]);

  const report = (value, label) => {
    if (!value) return;  // The file dialog was cancelled.
    setResult(value);
    if (value.added.length) notify(`${label}: ${value.added.join(', ')}`);
    else if (value.problems.length) notify(value.problems[0], true);
  };
  const addApk = () => run('sideload-add', async () => report(await call('sideloadAdd'), 'Added'));
  const addFolder = () => run('sideload-add', async () => {
    const folder = await call('chooseFolder');
    if (folder) report(await call('sideloadScan', folder), 'Added');
  });

  return <div className="page">
    <header className="page-head">
      <div>
        <h1 className="page-title">Unknown sources</h1>
        <div className="page-sub">{games.length} {games.length === 1 ? 'game' : 'games'} · not from the Meta store</div>
      </div>
    </header>
    <p className="us-intro">Games that don’t come from the Meta Quest store: builds you are testing before release, itch.io releases, ports.
      Add the game’s APK (or a folder holding it); expansion files (.obb) next to the APK come along. No Meta sign-in is needed.</p>

    <div className="toolbar">
      <button type="button" className="btn btn-primary" disabled={state.busy || pending.has('sideload-add')} onClick={addApk}><FilePlus2 />Add APK</button>
      <button type="button" className="btn btn-outline" disabled={state.busy || pending.has('sideload-add')} onClick={addFolder}><FolderPlus />Add folder</button>
    </div>

    {games.length
      ? <div className="us-list">{games.map(game => <Row key={game.id} game={game} disk={disk[game.id]} state={state} run={run} pending={pending} notify={notify} open={open} refreshDisk={refreshDisk} />)}</div>
      : <Empty title="No unknown-source games yet">Add an APK or a folder of builds to play it here.</Empty>}

    {result?.problems.length > 0 && <div className="us-results" role="status">
      <strong>Not added</strong>
      <ul>{result.problems.map(p => <li key={p}>{p}</li>)}</ul>
    </div>}
  </div>;
}

function Row({ game, disk, state, run, pending, notify, open, refreshDisk }) {
  const key = `sideload-${game.id}`;
  const busy = state.busy || pending.has(key) || state.running === game.id;
  const obbs = (game.files || []).filter(f => f.kind === 'obb').length;
  const changed = disk?.changed, missing = disk?.missing;
  const install = () => run(key, async () => { await call('sideloadInstall', game.id); await refreshDisk(); notify(`${game.name} is installed`); });
  const setOwned = owned => run(key, () => call('sideloadOwned', game.id, owned));
  const remove = () => run(key, async () => { await call('sideloadRemove', game.id); notify(`${game.name} removed from Refract. Files and the installed copy are untouched.`); });
  return <div className="us-row" data-game={game.id}>
    <div className="us-main">
      <Cover game={game} />
      <div className="us-text">
        <div className="us-name" title={game.name}>{game.name}</div>
        <div className="us-meta">{[game.package, game.version && `v${game.version}`, obbs > 0 && `${obbs} expansion ${obbs === 1 ? 'file' : 'files'}`].filter(Boolean).join(' · ')}</div>
        <div className="us-flags">
          <span className="us-flag"><GameStatus game={game} running={state.running === game.id} /></span>
          {changed && <span className="us-flag warn"><TriangleAlert size={12} /> APK changed since install</span>}
          {missing && <span className="us-flag warn"><TriangleAlert size={12} /> APK file not found</span>}
          {disk?.obbMissing?.length > 0 && <span className="us-flag warn"><TriangleAlert size={12} /> {disk.obbMissing.length} expansion {disk.obbMissing.length === 1 ? 'file' : 'files'} missing</span>}
        </div>
      </div>
      <div className="us-actions">
        <button type="button" className={`btn btn-sm ${changed || !game.installed ? 'btn-primary' : 'btn-outline'}`} disabled={busy || missing} onClick={install}>
          {game.installed ? <><RefreshCcw />{changed ? 'Update' : 'Reinstall'}</> : <><ArrowDownToLine />Install</>}
        </button>
        <button type="button" className="btn btn-sm btn-ghost" onClick={() => open(game.id)}>{game.installed ? 'Play / details' : 'Details'}</button>
        <IconButton label="Remove from Refract" disabled={busy} onClick={remove}><Trash2 /></IconButton>
      </div>
    </div>
    <div className="us-options">
      <label>
        <input type="checkbox" className="switch" checked={Boolean(game.owned)} disabled={busy} onChange={e => setOwned(e.target.checked)} />
        <span>Pass the game’s ownership check. For your own builds that check for an owner; most games don’t need it.</span>
      </label>
    </div>
  </div>;
}
