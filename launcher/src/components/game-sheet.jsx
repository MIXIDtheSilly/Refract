import { useState } from 'react';
import { Dialog } from 'radix-ui';
import { ArrowDownToLine, ExternalLink, FilePlus2, FolderOpen, History, Loader2, PackagePlus, Play, RefreshCcw, X } from 'lucide-react';
import { call } from '../api';
import { activeStatuses, ago, bytes, Cover, GameStatus, IconButton } from './common';

export function GameSheet({ game, state, onClose, run, pending, setPage, notify, connect }) {
  const [extra, setExtra] = useState(null);
  const [build, setBuild] = useState('');
  const key = `game-${game.id}`;
  const working = pending.has(key);
  const busy = state.busy || working;
  const activeJob = state.jobs.find(j => j.gameId === game.id && activeStatuses.includes(j.status));
  const running = state.running === game.id;
  const operate = task => run(key, task);
  const toDownloads = () => { onClose(); setPage('downloads'); };
  const download = () => operate(async () => { await call('download', game.id, build || undefined); toDownloads(); });
  const install = () => operate(async () => { await call('install', game.id); notify(`${game.name} is installed`); });
  const loadExtra = kind => operate(async () => {
    const items = await call(kind, game.id);
    setExtra({ kind, items });
    if (kind === 'builds') setBuild(items[0]?.id || '');
  });

  let primary;
  if (activeJob) primary = <button type="button" className="btn btn-primary btn-lg" onClick={toDownloads}><Loader2 className="spin" />{activeJob.status === 'downloading' ? 'Downloading' : activeJob.status === 'installing' ? 'Installing' : 'Queued'}</button>;
  else if (running) primary = <button type="button" className="btn btn-primary btn-lg" disabled={pending.has('stop')} onClick={() => run('stop', async () => { await call('stop'); notify('Closing game'); })}>Stop</button>;
  else if (game.installed) primary = <button type="button" className="btn btn-primary btn-lg" disabled={busy || Boolean(state.running)} onClick={() => operate(async () => { await call('play', game.id); onClose(); })}><Play fill="currentColor" />Play</button>;
  else if (game.apk) primary = <button type="button" className="btn btn-primary btn-lg" disabled={busy} onClick={install}><ArrowDownToLine />Install</button>;
  else if (!state.signedIn) primary = <button type="button" className="btn btn-primary btn-lg" disabled={pending.has('account')} onClick={connect}>Connect Meta to download</button>;
  else primary = <button type="button" className="btn btn-primary btn-lg" disabled={busy} onClick={download}><ArrowDownToLine />Download</button>;

  const manage = [];
  if (game.source === 'meta') manage.push(
    { icon: History, label: 'Versions', hint: 'Pick a Quest build', disabled: busy || !state.signedIn, onClick: () => loadExtra('builds') },
    { icon: PackagePlus, label: 'Add-ons', hint: 'DLC you own', disabled: busy || !state.signedIn, onClick: () => loadExtra('dlc') });
  if (game.apk) manage.push(
    { icon: FilePlus2, label: 'Add content files', hint: 'OBB / assets', disabled: busy, onClick: () => operate(async () => { await call('importAssets', game.id); notify('Install again to apply content files'); }) },
    ...(game.installed ? [{ icon: RefreshCcw, label: 'Update installation', hint: 'Reinstall, keeps saves', disabled: busy, onClick: install }] : []),
    { icon: FolderOpen, label: 'Open folder', disabled: false, onClick: () => operate(() => call('openFolder', game.id)) });
  if (!game.apk && game.source !== 'meta') manage.push({ icon: FilePlus2, label: 'Import APK', disabled: busy, onClick: () => run('import', () => call('import')) });
  if (game.source === 'meta') manage.push({ icon: ExternalLink, label: 'View on meta.com', disabled: false, onClick: () => operate(() => call('openStore', game.id)) });

  const meta = [
    game.publisher,
    game.version && `Version ${game.version}`,
    game.lastPlayed && `Played ${ago(game.lastPlayed)}`,
  ].filter(Boolean);

  return <Dialog.Root open onOpenChange={open => { if (!open) onClose(); }}>
    <Dialog.Portal>
      <Dialog.Overlay className="overlay" />
      <Dialog.Content className="sheet" id="details" aria-describedby={undefined}>
        <Dialog.Close asChild><IconButton label="Close" className="sheet-close"><X /></IconButton></Dialog.Close>
        <div className="sheet-scroll">
          <Cover game={game} />
          <div className="sheet-body">
            <Dialog.Title className="sheet-title">{game.name}</Dialog.Title>
            <div className="sheet-meta">
              <span className="status" style={{ textTransform: 'none', gap: 7 }}><GameStatus game={game} running={running} /></span>
              {meta.map(m => <span key={m}>{m}</span>)}
            </div>
            <div className="sheet-actions">{primary}</div>
            {working && <div className="working" role="status"><Loader2 className="spin" />Working…</div>}

            {extra?.kind === 'builds' && <div className="extra">
              <div className="extra-head">Versions</div>
              {extra.items.length ? <div className="field-row">
                <select className="field-input" aria-label="Quest build" value={build} onChange={e => setBuild(e.target.value)}>
                  {extra.items.map(b => <option key={b.id} value={b.id}>{b.version || b.code}</option>)}
                </select>
                <button type="button" className="btn btn-primary" disabled={busy || Boolean(activeJob)} onClick={download}>Download</button>
              </div> : <span style={{ color: 'var(--muted)' }}>No builds available</span>}
            </div>}
            {extra?.kind === 'dlc' && <div className="extra">
              <div className="extra-head">Add-ons</div>
              {extra.items.length ? extra.items.map(dlc => <div key={dlc.id} className="dlc">
                <div><div>{dlc.name}</div><small>{!dlc.owned ? 'Ownership not confirmed' : !dlc.fileCount ? 'Included in the game, nothing to download' : bytes(dlc.bytes)}</small></div>
                <button type="button" className="btn btn-outline btn-sm" disabled={!dlc.owned || !dlc.fileCount || busy || Boolean(activeJob)}
                  onClick={() => operate(async () => { await call('downloadDlc', game.id, dlc.id); toDownloads(); })}>Download</button>
              </div>) : <span style={{ color: 'var(--muted)' }}>No downloadable add-ons</span>}
            </div>}

            {game.description && <p className="description">{game.description}</p>}
            {game.genres?.length > 0 && <div className="tags">{game.genres.map(g => <span key={g} className="tag">{g}</span>)}</div>}

            {manage.length > 0 && <div className="manage">
              <h3 className="section-label">Manage</h3>
              <div className="manage-list">{manage.map(item => <button key={item.label} type="button" className="manage-item" disabled={item.disabled} onClick={item.onClick}>
                <item.icon /><span>{item.label}</span>{item.hint && <small>{item.hint}</small>}
              </button>)}</div>
            </div>}
          </div>
        </div>
      </Dialog.Content>
    </Dialog.Portal>
  </Dialog.Root>;
}
