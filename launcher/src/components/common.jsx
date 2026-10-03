import { useEffect, useState } from 'react';
import { LogoMark } from './logo';

export const activeStatuses = ['queued', 'downloading', 'installing'];

export function bytes(n) {
  if (!n) return '0 MB';
  return n >= 1024 ** 3 ? `${(n / 1024 ** 3).toFixed(n < 10 * 1024 ** 3 ? 2 : 1)} GB` : `${Math.max(1, Math.round(n / 1024 ** 2))} MB`;
}
export function ago(iso) {
  const minutes = Math.round((Date.now() - new Date(iso).getTime()) / 60000);
  if (!Number.isFinite(minutes)) return '';
  if (minutes < 2) return 'just now';
  if (minutes < 60) return `${minutes} minutes ago`;
  const hours = Math.round(minutes / 60);
  if (hours < 24) return `${hours} hour${hours === 1 ? '' : 's'} ago`;
  const days = Math.round(hours / 24);
  return days === 1 ? 'yesterday' : `${days} days ago`;
}

// Artwork only from Meta's CDNs or icons the backend read from Android.
function safeImage(value) {
  try {
    const url = new URL(value);
    if (url.protocol === 'data:' && value.startsWith('data:image/png;base64,')) return value;
    if (url.protocol === 'https:' && ['oculuscdn.com', 'fbcdn.net', 'oculus.com', 'meta.com'].some(d => url.hostname === d || url.hostname.endsWith(`.${d}`))) return value;
  } catch { /* Not a URL. */ }
  return '';
}

export function Cover({ game, className = '' }) {
  const src = safeImage(game.image);
  const [failed, setFailed] = useState(false);
  useEffect(() => setFailed(false), [src]);
  return <div className={`cover ${className}`}>
    {src && !failed ? <img src={src} alt="" loading="lazy" draggable="false" onError={() => setFailed(true)} />
      : <div className="cover-art"><LogoMark className="ring" /></div>}
  </div>;
}

export function IconButton({ label, children, className = '', ...props }) {
  return <button type="button" className={`icon-btn ${className}`} aria-label={label} title={label} {...props}>{children}</button>;
}

export function GameStatus({ game, running }) {
  if (running) return <><span className="dot live" />Playing</>;
  if (game.vrSdk === 'vrapi' && !game.installed) return <><span className="dot warn" />VrApi, not supported</>;
  if (game.installed) return <><span className="dot" />Installed</>;
  if (game.downloaded || game.apk) return <><span className="dot hollow" />Ready to install</>;
  if (game.source === 'meta' && game.owned === false) return 'Not owned';
  return 'In library';
}

export function Empty({ title, children, action }) {
  return <div className="empty">
    <LogoMark className="mark" />
    <h2>{title}</h2>
    {children && <p>{children}</p>}
    {action}
  </div>;
}
