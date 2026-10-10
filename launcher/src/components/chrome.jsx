import { ArrowDownToLine, Cpu, LayoutGrid, Loader2, Minus, Settings, Square, User, X } from 'lucide-react';
import { useState } from 'react';
import { windowControls } from '../api';
import { LogoMark } from './logo';

export function TitleBar() {
  if (!windowControls) return null;
  return <div className="titlebar">
    <div className="titlebar-drag" data-tauri-drag-region><LogoMark /><span data-tauri-drag-region>Refract</span></div>
    <div className="window-controls">
      <button type="button" aria-label="Minimize" onClick={windowControls.minimize}><Minus /></button>
      <button type="button" aria-label="Maximize" onClick={windowControls.toggleMaximize}><Square style={{ width: 13 }} /></button>
      <button type="button" aria-label="Close" className="close" onClick={windowControls.close}><X /></button>
    </div>
  </div>;
}

const pages = [
  ['library', 'Library', LayoutGrid],
  ['downloads', 'Downloads', ArrowDownToLine],
  ['emulator', 'Emulator', Cpu],
  ['settings', 'Settings', Settings],
];

function AccountAvatar({ name, image }) {
  const [failed, setFailed] = useState('');
  if (image && failed !== image) return <img className="avatar" src={image} alt="" referrerPolicy="no-referrer" onError={() => setFailed(image)} />;
  return <div className="avatar">{name ? name[0].toUpperCase() : <User />}</div>;
}

export function Sidebar({ page, setPage, state, activeJobs, connecting, onConnect }) {
  return <aside className="sidebar">
    <button type="button" className="brand" onClick={() => setPage('library')} aria-label="Refract library">
      <LogoMark /><span>Refract</span>
    </button>
    <nav className="nav" aria-label="Main navigation">
      {pages.filter(([id]) => id !== 'emulator' || state?.settings.backend !== 'native').map(([id, label, Icon]) => <button key={id} type="button" className="nav-item" data-nav={id}
        aria-current={page === id ? 'page' : undefined} onClick={() => setPage(id)}>
        <Icon />{label}
        {id === 'downloads' && activeJobs > 0 && <span className="nav-badge">{activeJobs}</span>}
      </button>)}
    </nav>
    <div className="sidebar-spacer" />
    {state && (state.signedIn
      ? <div className="account">
        <AccountAvatar name={state.account} image={state.accountImage} />
        <div className="account-text"><strong>{state.account || 'Meta account'}</strong><span>Connected to Meta</span></div>
      </div>
      : <div className="account account-signin">
        <div className="avatar"><User /></div>
        <div className="account-text"><strong>Meta</strong><span>Not connected</span></div>
        <button type="button" className="btn btn-primary btn-sm" disabled={connecting} onClick={onConnect}>
          {connecting ? <><Loader2 className="spin" />Waiting for Meta</> : 'Connect Meta'}
        </button>
      </div>)}
  </aside>;
}
