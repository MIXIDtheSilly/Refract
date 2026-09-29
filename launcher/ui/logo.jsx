import { useId } from 'react';
import { cn } from '@/lib/utils';

// Traced from img/Refract_logo.png (1024 px grid). The planet's outline is cut by a
// diagonal band through its centre; the ring is drawn everywhere except the back
// half inside the planet, so its front half crosses over the planet.
export function LogoMark({ className, title }) {
  const id = useId().replace(/[^\w-]/g, '');
  return <svg viewBox="56 196 928 632" className={cn('shrink-0', className)} fill="none" stroke="currentColor" strokeWidth="48"
    role={title ? 'img' : undefined} aria-label={title} aria-hidden={title ? undefined : true}>
    <defs>
      <mask id={`${id}-gap`} maskUnits="userSpaceOnUse" x="0" y="0" width="1024" height="1024">
        <rect width="1024" height="1024" fill="#fff" stroke="none" />
        <rect x="-600" y="-55" width="1200" height="110" fill="#000" stroke="none" transform="translate(518 511) rotate(-63.4)" />
      </mask>
      <mask id={`${id}-ring`} maskUnits="userSpaceOnUse" x="0" y="0" width="1024" height="1024">
        <rect width="1024" height="1024" fill="#fff" stroke="none" />
        <circle cx="518" cy="511" r="270" fill="#000" stroke="none" />
        <rect x="-500" y="0" width="1000" height="300" fill="#fff" stroke="none" transform="translate(519 522) rotate(-13)" />
      </mask>
    </defs>
    <circle cx="518" cy="511" r="281" mask={`url(#${id}-gap)`} />
    <g mask={`url(#${id}-ring)`}><ellipse cx="519" cy="522" rx="440" ry="83" transform="rotate(-13 519 522)" /></g>
  </svg>;
}

export function Wordmark({ className }) {
  return <span className={cn('flex items-center gap-2.5', className)}>
    <LogoMark className="h-[1.15em] w-auto" />
    <span className="font-display font-normal tracking-[0.01em]">Refract</span>
  </span>;
}
