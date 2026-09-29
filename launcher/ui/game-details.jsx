import { useRef, useState } from 'react';
import { Download, Ellipsis, ExternalLink, LoaderCircle, Play, Plus, Square } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogTitle } from '@/components/ui/dialog';
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuSeparator, DropdownMenuTrigger } from '@/components/ui/dropdown-menu';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { activeStatuses, ago, bytes, call, Cover, IconButton } from './common';

const sources = { meta: 'Meta Quest store', installed: 'Found in Android', local: 'Imported APK' };
function Fact({ label, children }) {
  return <div className="min-w-0"><dt className="text-xs text-muted-foreground">{label}</dt><dd className="mt-0.5 truncate">{children}</dd></div>;
}

export function GameDetails({ game, state, local, onClose, run, pending, setPage, notify }) {
  const returnFocus = useRef(document.activeElement);
  const [extra, setExtra] = useState(null);
  const [build, setBuild] = useState('');
  const busy = state.busy || pending.has(`game-${game.id}`);
  const activeJob = state.jobs.find(j => j.gameId === game.id && activeStatuses.includes(j.status));
  const running = state.running === game.id;
  const operate = fn => run(`game-${game.id}`, fn);
  const download = () => operate(async () => { await call('download', game.id, build || undefined); onClose(); setPage('downloads'); });
  const install = () => operate(async () => { await call('install', game.id); notify('Installed'); });
  const loadExtra = kind => operate(async () => { const items = await call(kind, game.id); setExtra({ kind, items }); if (kind === 'builds') setBuild(items[0]?.id || ''); });
  const size = game.files?.reduce((n, f) => n + Number(f.size || 0), 0);
  const status = running ? 'Playing' : game.installed ? 'Installed' : game.apk ? 'Downloaded' : local ? 'Not downloaded' : 'Not in library';
  return <Dialog open onOpenChange={open => { if (!open) onClose(); }}>
    <DialogContent id="details" aria-describedby={undefined} onCloseAutoFocus={event => { event.preventDefault(); if (returnFocus.current?.isConnected) returnFocus.current.focus(); }}
      className="max-h-[88vh] gap-0 overflow-x-hidden overflow-y-auto p-0 sm:max-w-[600px]">
      <div className="relative">
        <Cover game={game} className="aspect-[2/1] rounded-none rounded-t-2xl" />
        <div className="pointer-events-none absolute inset-x-0 bottom-0 h-28 bg-gradient-to-t from-popover to-transparent" />
      </div>
      <div className="relative -mt-8 px-6">
        <DialogTitle className="font-display text-[28px] leading-tight font-normal">{game.name}</DialogTitle>
        {(game.version || game.package) && <div className="mt-1 truncate text-xs text-muted-foreground">{[game.version && `Version ${game.version}`, game.package].filter(Boolean).join(' · ')}</div>}
      </div>
      <div className="space-y-5 p-6 pt-5">
        <div className="flex items-center gap-2">
          {activeJob ? <Button size="lg" variant="secondary" onClick={() => { onClose(); setPage('downloads'); }}><LoaderCircle className="animate-spin" />{activeJob.status === 'downloading' ? 'Downloading' : activeJob.status === 'installing' ? 'Installing' : 'Queued'}</Button>
            : running ? <Button size="lg" disabled={busy} onClick={() => operate(async () => { await call('stop'); notify('Closing game'); })}><Square className="fill-current" />Stop</Button>
            : game.installed ? <Button size="lg" className="min-w-32" disabled={busy || Boolean(state.running)} onClick={() => operate(async () => { await call('play', game.id); onClose(); })}><Play className="fill-current" />Play</Button>
            : game.apk ? <Button size="lg" disabled={busy} onClick={install}><Download />Install</Button>
            : !local ? <Button size="lg" disabled={busy} onClick={() => operate(() => call('add', game.id))}><Plus />Add to library</Button>
            : !state.signedIn ? <Button size="lg" disabled={pending.has('account')} onClick={() => run('account', () => call('login'))}>Connect Meta</Button>
            : <Button size="lg" disabled={busy} onClick={download}><Download />Download</Button>}
          {pending.has(`game-${game.id}`) && <span role="status" className="ml-2 flex items-center gap-2 text-xs text-muted-foreground"><LoaderCircle className="size-3.5 animate-spin" />Working…</span>}
          <div className="flex-1" />
          {game.source === 'meta' && <IconButton label="View on Meta" onClick={() => operate(() => call('openStore', game.id))}><ExternalLink /></IconButton>}
          {local && <DropdownMenu><DropdownMenuTrigger asChild><Button variant="outline" size="icon" aria-label="Game actions" title="Game actions"><Ellipsis /></Button></DropdownMenuTrigger>
            <DropdownMenuContent align="end">
              {game.source === 'meta' && <><DropdownMenuItem disabled={busy || !state.signedIn} onSelect={() => loadExtra('builds')}>Versions</DropdownMenuItem><DropdownMenuItem disabled={busy || !state.signedIn} onSelect={() => loadExtra('dlc')}>Add-ons</DropdownMenuItem></>}
              {game.apk && <>{game.source === 'meta' && <DropdownMenuSeparator />}<DropdownMenuItem disabled={busy} onSelect={() => operate(async () => { await call('patch', game.id); notify('Patched'); })}>Patch with ovrport</DropdownMenuItem><DropdownMenuItem disabled={busy} onSelect={() => operate(async () => { await call('importAssets', game.id); notify('Install to apply content files'); })}>Add content files</DropdownMenuItem><DropdownMenuItem onSelect={() => operate(() => call('openFolder', game.id))}>Open folder</DropdownMenuItem>{game.installed && <DropdownMenuItem disabled={busy} onSelect={install}>Update installation</DropdownMenuItem>}</>}
              {!game.apk && game.source !== 'meta' && <DropdownMenuItem disabled={busy} onSelect={() => run('import', () => call('import'))}>Import APK</DropdownMenuItem>}
            </DropdownMenuContent>
          </DropdownMenu>}
        </div>
        {local && <dl className="grid grid-cols-3 gap-4 rounded-xl bg-black/15 p-4 text-[13px]">
          <Fact label="Status">{status}</Fact>
          <Fact label="Source">{sources[game.source] || 'Local'}</Fact>
          {game.lastPlayed ? <Fact label="Last played">{ago(game.lastPlayed)}</Fact> : size ? <Fact label="Size">{bytes(size)}</Fact> : <Fact label="Patched">{game.patched ? 'Yes' : 'No'}</Fact>}
        </dl>}
        {!local && game.price !== undefined && <p className="text-[13px] text-muted-foreground">{!game.price || parseFloat(String(game.price).replace(/[^\d.]/g, '')) === 0 ? 'Free on the Meta Quest store' : `${game.price} on the Meta Quest store`}</p>}
        {extra?.kind === 'builds' && <div className="flex items-center gap-2 border-t border-white/10 pt-5">{extra.items.length ? <><Select value={build} onValueChange={setBuild}><SelectTrigger className="min-w-0 flex-1" aria-label="Quest build"><SelectValue /></SelectTrigger><SelectContent>{extra.items.map(b => <SelectItem key={b.id} value={b.id}>{b.version || b.code}</SelectItem>)}</SelectContent></Select><Button disabled={busy || Boolean(activeJob)} onClick={download}>Download</Button></> : <span className="text-muted-foreground">No builds available</span>}</div>}
        {extra?.kind === 'dlc' && <div className="border-t border-white/10 pt-5">{extra.items.length ? <><p className="mb-3 text-xs text-muted-foreground">Install after downloading to apply add-ons.</p>{extra.items.map(dlc => <div key={dlc.id} className="flex items-center justify-between gap-3 py-2"><div className="min-w-0"><div className="text-sm">{dlc.name}</div><div className="mt-1 text-xs text-muted-foreground">{!dlc.owned ? 'Ownership not confirmed' : !dlc.fileCount ? 'No separate download' : bytes(dlc.bytes)}</div></div><Button size="sm" variant="outline" disabled={!dlc.owned || !dlc.fileCount || busy || Boolean(activeJob)} onClick={() => operate(async () => { await call('downloadDlc', game.id, dlc.id); onClose(); setPage('downloads'); })}>Download</Button></div>)}</> : <p className="text-muted-foreground">No downloadable add-ons</p>}</div>}
      </div>
    </DialogContent>
  </Dialog>;
}
