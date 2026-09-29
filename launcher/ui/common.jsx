import { useEffect, useState } from 'react';
import { Check } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { cn } from '@/lib/utils';
import { LogoMark } from './logo';

export const activeStatuses = ['queued', 'downloading', 'installing', 'patching'];
export const bytes = n => n ? `${(n / 1024 ** 3).toFixed(n < 1024 ** 3 ? 2 : 1)} GB` : '0 GB';
export async function call(method, ...args) {
  const result = await window.refract[method](...args);
  if (!result.ok) throw new Error(result.error);
  return result.value;
}
export function safeImage(value) {
  try {
    const url = new URL(value);
    if (url.protocol === 'data:' && value.startsWith('data:image/png;base64,')) return value;
    if (url.protocol === 'https:' && ['oculuscdn.com', 'fbcdn.net', 'oculus.com', 'meta.com'].some(d => url.hostname === d || url.hostname.endsWith(`.${d}`))) return value;
  } catch {}
  return '';
}
const initials = name => name?.split(/\s+/).filter(Boolean).slice(0, 2).map(w => w[0]).join('').toUpperCase();
export function ago(iso) {
  const seconds = (Date.now() - new Date(iso)) / 1000;
  if (!(seconds >= 0)) return '';
  for (const [unit, size] of [['day', 86400], ['hour', 3600], ['minute', 60]]) if (seconds >= size) {
    const n = Math.floor(seconds / size); return `${n} ${unit}${n > 1 ? 's' : ''} ago`;
  }
  return 'just now';
}

export function IconButton({ label, children, ...props }) {
  return <Button variant="ghost" size="icon" aria-label={label} title={label} {...props}>{children}</Button>;
}
export function Cover({ game, className, imageClassName }) {
  const src = safeImage(game.image);
  const [failed, setFailed] = useState(false);
  useEffect(() => setFailed(false), [src]);
  return <div className={cn('relative flex aspect-[4/3] items-center justify-center overflow-hidden rounded-xl bg-gradient-to-br from-[#3d3d3d] to-[#2f2f2f]', className)}>
    {src && !failed ? <img src={src} alt="" loading="lazy" className={cn('h-full w-full object-cover', imageClassName)} onError={() => setFailed(true)} />
      : <span className="font-display text-4xl font-light tracking-wide text-white/35" aria-hidden="true">{initials(game.name)}</span>}
  </div>;
}
export function StatusLabel({ game, running, store }) {
  if (store) return !game.price || parseFloat(String(game.price).replace(/[^\d.]/g, '')) === 0 ? 'Free' : game.price;
  if (running) return <><span className="size-1.5 animate-pulse rounded-full bg-white" />Playing</>;
  if (game.installed) return <><Check className="size-3" />Installed</>;
  if (game.downloaded || game.apk) return 'Downloaded';
  return game.source === 'meta' ? 'In your library' : null;
}
export function GameGrid({ games, onOpen, running, store = false }) {
  return <div className="grid grid-cols-[repeat(auto-fill,minmax(176px,1fr))] gap-x-5 gap-y-7">
    {games.map(game => <button key={game.id} data-game={game.id} data-store={store || undefined} onClick={() => onOpen(game.id)} aria-label={`Open ${game.name}`}
      className="group min-w-0 rounded-xl text-left outline-none focus-visible:outline-none">
      <Cover game={game} className="shadow-[0_2px_10px_rgb(0_0_0/0.25)] ring-1 ring-white/[0.06] transition-[box-shadow,transform] duration-200 group-hover:-translate-y-0.5 group-hover:ring-2 group-hover:ring-white/60 group-focus-visible:ring-2 group-focus-visible:ring-white"
        imageClassName="transition-transform duration-300 group-hover:scale-[1.03]" />
      <div className="mt-2.5 truncate font-medium" title={game.name}>{game.name}</div>
      <div className="mt-0.5 flex min-h-5 items-center gap-1.5 text-xs text-muted-foreground"><StatusLabel game={game} running={running === game.id} store={store} /></div>
    </button>)}
  </div>;
}
export function PageHeader({ title, subtitle, children }) {
  return <div className="mb-6 flex min-h-12 flex-wrap items-end gap-x-6 gap-y-3">
    <div className="min-w-0"><h1 className="font-display text-[30px] leading-none font-normal tracking-[0.005em]">{title}</h1>{subtitle && <p className="mt-2 text-[13px] text-muted-foreground">{subtitle}</p>}</div>
    {children && <div className="ml-auto flex items-center gap-2">{children}</div>}
  </div>;
}
export function Empty({ children, hint, action }) {
  return <div className="flex flex-col items-center py-20 text-center">
    <LogoMark className="mb-5 h-12 w-auto text-white/[0.12]" />
    <p className="text-[15px] text-foreground/80">{children}</p>
    {hint && <p className="mt-1.5 max-w-sm text-[13px] text-muted-foreground">{hint}</p>}
    {action && <div className="mt-5">{action}</div>}
  </div>;
}
