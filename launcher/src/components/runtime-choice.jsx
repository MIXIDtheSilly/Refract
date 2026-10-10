import { Cpu, Zap } from 'lucide-react';

export const runtimes = [
  { id: 'emulator', title: 'Android emulator', icon: Cpu, tag: 'Most compatible',
    text: 'Runs a virtual Android device with Refract’s ARM translator. Plays on a VR headset or in a window on this PC. Needs the Android SDK and Windows hardware acceleration.' },
  { id: 'native', title: 'Refract Native', icon: Zap, tag: 'Experimental',
    text: 'Runs games directly on this PC with no emulator: faster to start and lighter on memory. Plays in a window on this PC (no VR headset yet), and some games may not work. Needs Quest system files.' },
];

// Two cards to pick how Refract runs games. Used by the first-run wizard and Settings > Runtime.
export function RuntimeChoice({ value, onChange, disabled }) {
  return <div className="runtime-choice" role="radiogroup" aria-label="How Refract runs games">
    {runtimes.map(({ id, title, icon: Icon, tag, text }) => <button key={id} type="button" role="radio" aria-checked={value === id} disabled={disabled}
      className={`runtime-card ${value === id ? 'selected' : ''}`} onClick={() => onChange(id)}>
      <Icon /><div><strong>{title}</strong><small>{tag}</small><span>{text}</span></div>
    </button>)}
  </div>;
}
