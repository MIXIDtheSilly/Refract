import { useState } from 'react';
import { Loader2 } from 'lucide-react';
import { call } from '../api';
import { LogoMark } from './logo';
import { RuntimeChoice } from './runtime-choice';
import { SetupPanel } from './setup';

// First run: choose how Refract runs games, then get that runtime ready. Settings > Runtime changes it later.
export function Welcome({ state, run, pending, notify }) {
  const [backend, setBackend] = useState(state.settings.backend || 'emulator');
  const [step, setStep] = useState(0);
  const save = (setupDone, next) => run('welcome', async () => {
    await call('settings', { ...state.settings, backend, setupDone });
    next?.();
  });
  return <div className="welcome" role="dialog" aria-modal="true" aria-labelledby="welcome-title">
    <div className="welcome-card">
      <span className="orbit"><LogoMark /></span>
      <h1 id="welcome-title">Welcome to Refract</h1>
      {step === 0 ? <>
        <p>Choose how Refract should run Quest games on this PC. You can change this any time in Settings.</p>
        <RuntimeChoice value={backend} onChange={setBackend} />
        <div className="welcome-actions">
          <button type="button" className="btn btn-ghost" disabled={pending.has('welcome')} onClick={() => save(true)}>Skip setup</button>
          <button type="button" className="btn btn-primary" disabled={pending.has('welcome')} onClick={() => save(false, () => setStep(1))}>
            {pending.has('welcome') && <Loader2 className="spin" />}Continue</button>
        </div>
      </> : <>
        <SetupPanel state={state} run={run} pending={pending} notify={notify} />
        <div className="welcome-actions">
          <button type="button" className="btn btn-ghost" onClick={() => setStep(0)}>Back</button>
          <button type="button" className="btn btn-primary" disabled={pending.has('welcome')} onClick={() => save(true)}>Finish</button>
        </div>
      </>}
    </div>
  </div>;
}
