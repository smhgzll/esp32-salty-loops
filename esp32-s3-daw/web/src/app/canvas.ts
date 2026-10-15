// SPDX-License-Identifier: GPL-3.0-or-later
import { useEffect, useRef } from 'preact/hooks';
import { live } from './state';

/** One observer/subscription per mounted editor; live drawing at most once per paint. */
export function useCanvasLive(wrap: { current: HTMLDivElement | null }, resize: (el: HTMLDivElement) => void, playhead: () => void) {
  const latest = useRef({ resize, playhead });
  latest.current = { resize, playhead };
  useEffect(() => {
    const el = wrap.current;
    if (!el) return;
    let frame = 0;
    let last = '';
    const paint = () => {
      frame = 0;
      latest.current.playhead();
    };
    const schedule = () => {
      const s = live.state.status;
      const key = `${s?.tick}/${s?.play}/${s?.mode}/${s?.pattern}`;
      if (key === last || document.hidden) return;
      last = key;
      if (!frame) frame = requestAnimationFrame(paint);
    };
    const visible = () => { last = ''; schedule(); };
    const ro = new ResizeObserver(() => latest.current.resize(el));
    ro.observe(el);
    const unsub = live.subscribe(schedule);
    document.addEventListener('visibilitychange', visible);
    return () => {
      if (frame) cancelAnimationFrame(frame);
      ro.disconnect();
      unsub();
      document.removeEventListener('visibilitychange', visible);
    };
  }, []);
}
