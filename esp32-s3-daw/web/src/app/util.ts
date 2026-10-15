// SPDX-License-Identifier: GPL-3.0-or-later
export const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
/** FL-style octave numbering: MIDI 60 = C5 */
export const noteName = (k: number) => `${NOTE_NAMES[k % 12]}${Math.floor(k / 12)}`;
export const isBlack = (k: number) => [1, 3, 6, 8, 10].includes(k % 12);
export const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);

export function bbt(tick: number, ppq: number, ts: [number, number]) {
  const beat = (ppq * 4) / ts[1];
  const bar = beat * ts[0];
  const b = Math.floor(tick / bar);
  const inBar = tick - b * bar;
  const bt = Math.floor(inBar / beat);
  const t = Math.floor(inBar - bt * beat);
  return { bar: b + 1, beat: bt + 1, tick: t };
}
export const fmtBbt = (tick: number, ppq: number, ts: [number, number]) => {
  const x = bbt(tick, ppq, ts);
  return `${String(x.bar).padStart(3, ' ')}:${String(x.beat).padStart(2, '0')}:${String(x.tick).padStart(3, '0')}`;
};

export function hexToRgb(h: string): [number, number, number] {
  const v = parseInt(h.slice(1), 16) || 0;
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
}
export const rgba = (h: string, a: number) => {
  const [r, g, b] = hexToRgb(h);
  return `rgba(${r},${g},${b},${a})`;
};
export function shade(h: string, f: number) {
  const [r, g, b] = hexToRgb(h).map((c) => clamp(Math.round(f >= 0 ? c + (255 - c) * f : c * (1 + f)), 0, 255));
  return `#${((r << 16) | (g << 8) | b).toString(16).padStart(6, '0')}`;
}

export const PALETTE = ['#ff6b4a', '#ffb03a', '#f5d547', '#c6e04a', '#4cd964', '#4cd9a0', '#40c4d9', '#5fb3ff', '#7b8cff', '#9a7bff',
  '#d97bff', '#ff7ad9', '#ff7a8a', '#b08a6a', '#8a9aa8', '#c8c8c8'];

/** gain (linear) -> dB text */
export const dbText = (g: number) => (g <= 0.00001 ? '-inf' : `${(20 * Math.log10(g)).toFixed(1)}`);

export function throttle<A extends unknown[]>(fn: (...a: A) => void, ms: number) {
  let last = 0;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let pending: A | null = null;
  const run = () => {
    timer = null;
    last = Date.now();
    if (pending) fn(...pending);
    pending = null;
  };
  return (...a: A) => {
    pending = a;
    const wait = ms - (Date.now() - last);
    if (wait <= 0) run();
    else if (!timer) timer = setTimeout(run, wait);
  };
}

export const isTouch = () => typeof window !== 'undefined' && matchMedia('(pointer: coarse)').matches;
