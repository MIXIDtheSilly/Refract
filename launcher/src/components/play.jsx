import { useCallback, useEffect, useState } from 'react';
import { Loader2, Monitor, RectangleGoggles, RefreshCw } from 'lucide-react';
import { call } from '../api';
import { IconButton } from './common';

// Whether a VR headset is ready: checked at once, then every 15 s and on focus while no game runs
// (a running session owns the headset). { connected, runtime, title?, detail? } or null before the first answer.
export function useHeadset(state) {
  const [headset, setHeadset] = useState(null);
  const [checking, setChecking] = useState(false);
  const idle = Boolean(state) && !state.running && !state.starting;
  const check = useCallback(async () => {
    setChecking(true);
    try { setHeadset(await call('headset')); } catch { /* Keep the last answer. */ }
    finally { setChecking(false); }
  }, []);
  useEffect(() => {
    if (!idle) return undefined;
    check();
    const timer = setInterval(() => { if (document.visibilityState === 'visible') check(); }, 15000);
    window.addEventListener('focus', check);
    return () => { clearInterval(timer); window.removeEventListener('focus', check); };
  }, [idle, check]);
  return { headset, checking, check };
}

export const headsetMissing = headset => Boolean(headset && !headset.connected && !headset.busy);

// Headset status pill with a button to check again; the pill's tooltip says how to connect.
export function HeadsetStatus({ headset, checking, check }) {
  if (!headset || headset.busy) return null;
  return <div className="headset-status" role="status" aria-live="polite">
    <span className={`headset-chip ${headset.connected ? 'ready' : ''}`} title={headset.connected ? `Connected through ${headset.runtime}` : headset.detail}>
      <span className="dot" />{headset.connected ? 'VR headset ready' : 'No VR headset'}
    </span>
    <IconButton label="Check for a VR headset" className="icon-btn-sm" disabled={checking} onClick={check}><RefreshCw className={checking ? 'spin' : ''} /></IconButton>
  </div>;
}

// Play in VR and Play on PC for an installed game, or its progress/Stop once started. The one that will work now
// comes first: VR when a headset is ready (or not known yet), PC when none is connected.
export function PlayButtons({ game, state, run, pending, notify, headset, onStarted = () => {} }) {
  const key = `game-${game.id}`;
  if (state.running === game.id) {
    return <button type="button" className="btn btn-primary btn-lg" disabled={pending.has('stop')} onClick={() => run('stop', async () => { await call('stop'); notify('Closing game'); })}>Stop</button>;
  }
  if (state.starting?.gameId === game.id) return <button type="button" className="btn btn-primary btn-lg" disabled><Loader2 className="spin" />{state.starting.stage}</button>;
  const disabled = Boolean(state.running) || Boolean(state.starting) || state.busy || pending.has(key);
  const play = mode => run(key, async () => {
    await call('play', game.id, mode);
    if (mode === 'pc') notify('Playing on PC. Hold the right mouse button to look around; WASD walks. Controls are in the game’s details.');
    onStarted(mode);
  });
  // Refract Native has no headset path yet.
  if (state.settings.backend === 'native') return <button type="button" className="btn btn-lg btn-primary" disabled={disabled} onClick={() => play('pc')}><Monitor />Play on PC</button>;
  const vrFirst = !headsetMissing(headset);
  const vr = <button key="vr" type="button" className={`btn btn-lg ${vrFirst ? 'btn-primary' : 'btn-outline'}`} disabled={disabled} onClick={() => play('vr')}><RectangleGoggles />Play in VR</button>;
  const pc = <button key="pc" type="button" className={`btn btn-lg ${vrFirst ? 'btn-outline' : 'btn-primary'}`} disabled={disabled} onClick={() => play('pc')}><Monitor />Play on PC</button>;
  return vrFirst ? <>{vr}{pc}</> : <>{pc}{vr}</>;
}

// scripts/pose_input_server.py and the viewer's mouse handling. Keys work while the game window is in front.
const controls = [
  ['Look around', 'Hold right mouse button and drag'],
  ['Walk', 'W A S D (Shift runs)'],
  ['Right trigger / grip', 'Left click or E / middle click or R'],
  ['Left trigger / grip', 'Q / F'],
  ['A / B', 'Space / Backspace'],
  ['X / Y', 'Z / X'],
  ['Menu', 'Tab or M'],
  ['Right thumbstick', 'Arrow keys'],
  ['Hand distance', 'Mouse wheel'],
  ['Reach for what you look at', 'Hold C'],
  ['Recenter / fullscreen', 'Home / F11'],
  ['Xbox controller', 'Works like Quest controllers'],
];
export function PcControls() {
  return <details className="extra pc-controls">
    <summary className="extra-head">Play on PC controls</summary>
    <dl>{controls.map(([action, keys]) => <div key={action}><dt>{action}</dt><dd>{keys}</dd></div>)}</dl>
  </details>;
}
