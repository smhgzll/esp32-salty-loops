// SPDX-License-Identifier: GPL-3.0-or-later
// On-screen keyboard (touch and mouse, multi-touch chords, glissando by sliding) and the computer
// keyboard mapping (two rows from Z and Q, like most DAWs). Plays the selected channel.
import { useEffect, useRef, useState } from 'preact/hooks';

import * as A from '../app/actions';
import { ui } from '../app/state';
import { useStore } from '../app/store';
import { isBlack, noteName } from '../app/util';
import { Btn } from './controls';

const WHITE = 24;
export function VirtualKeyboard(p: { height?: number }) {
  const ch = useStore(ui, (s) => s.selectedChannel);
  const [oct, setOct] = useState(4);
  const [down, setDown] = useState<Set<number>>(new Set());
  const pointers = useRef(new Map<number, number>());
  const box = useRef<HTMLDivElement>(null);
  const lo = oct * 12, hi = lo + 36;
  const keys: { k: number; x: number; w: number; black: boolean }[] = [];
  let wx = 0;
  for (let k = lo; k <= hi; k++) {
    if (isBlack(k)) keys.push({ k, x: wx * WHITE - WHITE * 0.32, w: WHITE * 0.64, black: true });
    else keys.push({ k, x: wx++ * WHITE, w: WHITE, black: false });
  }
  const keyAt = (e: PointerEvent) => {
    const el = document.elementFromPoint(e.clientX, e.clientY) as HTMLElement | null;
    const k = el?.dataset?.key;
    return k ? +k : -1;
  };
  const press = (id: number, k: number, vel: number) => {
    const prev = pointers.current.get(id);
    if (prev === k) return;
    if (prev !== undefined && prev >= 0) A.noteOff(ch, prev);
    pointers.current.set(id, k);
    if (k >= 0) A.noteOn(ch, k, vel);
    setDown(new Set([...pointers.current.values()].filter((x) => x >= 0)));
  };
  const release = (id: number) => {
    const prev = pointers.current.get(id);
    if (prev !== undefined && prev >= 0) A.noteOff(ch, prev);
    pointers.current.delete(id);
    setDown(new Set([...pointers.current.values()].filter((x) => x >= 0)));
  };
  return (
    <div class="row" style={{ gap: 0, alignItems: 'stretch', background: '#0b0d0f' }}>
      <div style={{ display: 'flex', flexDirection: 'column', gap: '2px', padding: '4px' }}>
        <Btn class="icon" title="Octave up" onClick={() => setOct(Math.min(8, oct + 1))}>+</Btn>
        <span class="hint" style={{ textAlign: 'center' }}>C{oct}</span>
        <Btn class="icon" title="Octave down" onClick={() => setOct(Math.max(0, oct - 1))}>−</Btn>
      </div>
      <div class="vkb" style={{ height: `${p.height ?? 96}px`, flex: 1 }} ref={box}
        onPointerDown={(e) => {
          (e.currentTarget as Element).setPointerCapture(e.pointerId);
          const r = (e.target as HTMLElement).getBoundingClientRect();
          press(e.pointerId, keyAt(e), Math.min(1, Math.max(0.3, (e.clientY - r.top) / r.height + 0.2)));
        }}
        onPointerMove={(e) => pointers.current.has(e.pointerId) && press(e.pointerId, keyAt(e), 0.8)}
        onPointerUp={(e) => release(e.pointerId)}
        onPointerCancel={(e) => release(e.pointerId)}
      >
        <div class="vkb-inner" style={{ width: `${wx * WHITE}px` }}>
          {keys.map((k) => (
            <div key={k.k} data-key={k.k} class={`vkey ${k.black ? 'b' : 'w'}${down.has(k.k) ? ' down' : ''}`} style={{ left: `${k.x}px`, width: `${k.w}px` }}>
              {!k.black && k.k % 12 === 0 ? <span class="vkey-lbl" data-key={k.k}>{noteName(k.k)}</span> : null}
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

// computer keyboard: Z S X D C V G B H N J M (lower octave), Q 2 W 3 E R 5 T 6 Y 7 U (upper)
const LOWER = 'zsxdcvgbhnjm', UPPER = 'q2w3er5t6y7u';
export function useComputerKeyboard() {
  useEffect(() => {
    const held = new Map<string, number>();
    const base = () => 48;
    const keyOf = (e: KeyboardEvent) => {
      const c = e.key.toLowerCase();
      const a = LOWER.indexOf(c), b = UPPER.indexOf(c);
      return a >= 0 ? base() + a : b >= 0 ? base() + 12 + b : -1;
    };
    const editable = (e: KeyboardEvent) => {
      const t = e.target as HTMLElement;
      return t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA' || t.closest('.cv-wrap');
    };
    const down = (e: KeyboardEvent) => {
      if (e.repeat || e.ctrlKey || e.metaKey || e.altKey || editable(e)) return;
      const k = keyOf(e);
      if (k < 0 || held.has(e.code)) return;
      held.set(e.code, k);
      A.noteOn(ui.state.selectedChannel, k, 0.8);
    };
    const up = (e: KeyboardEvent) => {
      const k = held.get(e.code);
      if (k === undefined) return;
      held.delete(e.code);
      A.noteOff(ui.state.selectedChannel, k);
    };
    const blur = () => {
      for (const k of held.values()) A.noteOff(ui.state.selectedChannel, k);
      held.clear();
    };
    window.addEventListener('keydown', down);
    window.addEventListener('keyup', up);
    window.addEventListener('blur', blur);
    return () => {
      window.removeEventListener('keydown', down);
      window.removeEventListener('keyup', up);
      window.removeEventListener('blur', blur);
    };
  }, []);
}
