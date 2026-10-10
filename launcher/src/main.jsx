import { useCallback, useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { Loader2, X } from 'lucide-react';
import { call, native, onChange, onLaunchError } from './api';
import { Sidebar, TitleBar } from './components/chrome';
import { activeStatuses, Empty } from './components/common';
import { GameSheet } from './components/game-sheet';
import { useHeadset } from './components/play';
import { LogoMark } from './components/logo';
import { Library } from './pages/library';
import { Downloads } from './pages/downloads';
import { SettingsPage } from './pages/settings';
import { EmulatorPage } from './pages/emulator';
import { Welcome } from './components/welcome';
import './styles.css';

function App() {
  const [state, setState] = useState(null);
  const [page, setPage] = useState('library');
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
  // Runs one task per key; its key shows as pending until it settles.
  const run = useCallback(async (key, task) => {
    if (pendingRef.current.has(key)) return;
    pendingRef.current.add(key); setPending(new Set(pendingRef.current));
    try { return await task(); } catch (error) { notify(String(error?.message || error), true); }
    finally { pendingRef.current.delete(key); setPending(new Set(pendingRef.current)); }
  }, [notify]);

  useEffect(() => {
    let active = true;
    const off = onChange(next => { if (active) setState(next); });
    const offError = onLaunchError(error => notify(error, true));
    call('state').then(value => { if (active) setState(value); }).catch(error => notify(String(error?.message || error), true));
    return () => { active = false; off(); offError(); clearTimeout(timer.current); };
  }, [notify]);
  useEffect(() => { content.current?.scrollTo({ top: 0 }); }, [page]);

  const connect = () => run('account', async () => {
    const result = await call('login');
    notify(result?.partial ? 'Connected. Meta returned only part of your library.' : 'Connected to Meta');
  });
  const game = state?.games.find(g => g.id === selected);
  const activeJobs = state?.jobs.filter(j => activeStatuses.includes(j.status)).length || 0;
  const running = state?.running && state.games.find(g => g.id === state.running);
  const headset = useHeadset(state);
  const shared = { state, run, pending, notify, setPage, connect, open: setSelected, headset };

  return <div className={`app ${native ? 'native' : 'browser'}`}>
    <TitleBar />
    <div className="body">
      <Sidebar page={page} setPage={setPage} state={state} activeJobs={activeJobs} connecting={pending.has('account')} onConnect={connect} />
      <main className="content" ref={content} aria-label={page}>
        {!state ? <Empty title="Loading library" action={<Loader2 className="spin" />} />
          : page === 'library' ? <Library {...shared} />
          : page === 'downloads' ? <Downloads {...shared} />
          : page === 'emulator' && state.settings.backend !== 'native' ? <EmulatorPage {...shared} />
          : <SettingsPage key={JSON.stringify(state.settings)} {...shared} />}
      </main>
    </div>

    {state && !state.settings.setupDone && <Welcome state={state} run={run} pending={pending} notify={notify} />}
    {running && <div className="now-playing" role="status">
      <span className="orbit"><LogoMark /></span>
      <div><small>Now playing {state.runningMode === 'pc' ? 'on PC' : 'in VR'}</small><strong>{running.name}</strong></div>
      <button type="button" className="btn btn-sm" disabled={pending.has('stop')} onClick={() => run('stop', async () => { await call('stop'); notify('Closing game'); })}>Stop</button>
    </div>}
    {notice && <div role={notice.error ? 'alert' : 'status'} className={`toast ${notice.error ? 'error' : ''}`}>
      <span className="toast-text">{notice.text}</span>
      <button type="button" aria-label="Dismiss notification" onClick={() => setNotice(null)}><X /></button>
    </div>}
    {game && state && <GameSheet key={game.id} game={game} onClose={() => setSelected(null)} {...shared} />}
  </div>;
}

createRoot(document.getElementById('root')).render(<App />);
