// SPDX-License-Identifier: GPL-3.0-or-later
// Shared controls: knob, fader, meter, numeric drag field, toggle, menus, sheets, icons.
// Pointer Events throughout (mouse, pen and touch alike); no hover-only interaction.
import type { ComponentChildren, JSX } from 'preact';
import { useEffect, useRef, useState } from 'preact/hooks';

import { clamp } from '../app/util';

// ---- value scaling --------------------------------------------------------------------------
export type Scale = 'linear' | 'log' | 'db' | 'bipolar';
export function toNorm(v: number, min: number, max: number, scale: Scale) {
  if (scale === 'log' && min > 0) return Math.log(v / min) / Math.log(max / min);
  return (v - min) / (max - min || 1);
}
export function fromNorm(n: number, min: number, max: number, scale: Scale) {
  n = clamp(n, 0, 1);
  if (scale === 'log' && min > 0) return min * Math.pow(max / min, n);
  return min + (max - min) * n;
}

// ---- drag helper: vertical drag changes a normalised value ----------------------------------
function useDrag(onDelta: (dy: number, fine: boolean) => void, onEnd?: () => void) {
  const st = useRef<{ y: number; id: number } | null>(null);
  return {
    onPointerDown: (e: PointerEvent) => {
      if (e.button !== 0) return;
      (e.currentTarget as Element).setPointerCapture(e.pointerId);
      st.current = { y: e.clientY, id: e.pointerId };
      e.preventDefault();
      e.stopPropagation();
    },
    onPointerMove: (e: PointerEvent) => {
      if (!st.current || st.current.id !== e.pointerId) return;
      const dy = st.current.y - e.clientY;
      st.current.y = e.clientY;
      if (dy) onDelta(dy, e.shiftKey);
    },
    onPointerUp: (e: PointerEvent) => {
      if (st.current?.id === e.pointerId) {
        st.current = null;
        onEnd?.();
      }
    },
    onPointerCancel: () => {
      st.current = null;
      onEnd?.();
    },
  };
}

// ---- knob -------------------------------------------------------------------------------------
export interface KnobProps {
  value: number;
  min: number;
  max: number;
  def?: number;
  scale?: Scale;
  step?: number; // integer params
  size?: number;
  label?: string;
  color?: string;
  format?: (v: number) => string;
  onChange: (v: number, final: boolean) => void;
  title?: string;
}
export function Knob(p: KnobProps) {
  const scale = p.scale ?? 'linear';
  const size = p.size ?? 34;
  const norm = clamp(toNorm(p.value, p.min, p.max, scale), 0, 1);
  const acc = useRef(norm);
  acc.current = norm;
  const emit = (n: number, final: boolean) => {
    let v = fromNorm(n, p.min, p.max, scale);
    if (p.step) v = Math.round(v / p.step) * p.step;
    p.onChange(clamp(v, p.min, p.max), final);
  };
  const drag = useDrag(
    (dy, fine) => {
      acc.current = clamp(acc.current + dy * (fine ? 0.001 : 0.006), 0, 1);
      emit(acc.current, false);
    },
    () => emit(acc.current, true),
  );
  const a0 = -135, a1 = 135;
  const bip = scale === 'bipolar';
  const ang = a0 + (a1 - a0) * norm;
  const r = size / 2 - 3;
  const arc = (from: number, to: number) => {
    const p0 = polar(size / 2, size / 2, r, from), p1 = polar(size / 2, size / 2, r, to);
    const large = Math.abs(to - from) > 180 ? 1 : 0;
    return `M${p0.x},${p0.y} A${r},${r} 0 ${large} ${to > from ? 1 : 0} ${p1.x},${p1.y}`;
  };
  const tip = polar(size / 2, size / 2, r - 4, ang);
  return (
    <div class="knob" title={p.title ?? (p.label ? `${p.label}: ${p.format ? p.format(p.value) : p.value.toFixed(2)}` : '')}>
      <svg
        width={size}
        height={size}
        {...drag}
        onDblClick={() => p.def !== undefined && p.onChange(p.def, true)}
        onWheel={(e: WheelEvent) => {
          e.preventDefault();
          emit(clamp(norm + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 0.005 : 0.03), 0, 1), true);
        }}
        style={{ touchAction: 'none' }}
        role="slider"
        aria-label={p.label}
        aria-valuenow={p.value}
      >
        <circle cx={size / 2} cy={size / 2} r={r - 2} class="knob-body" />
        <path d={arc(a0, a1)} class="knob-track" />
        {norm > 0.001 || bip ? <path d={bip ? arc(Math.min(0, ang), Math.max(0, ang)) : arc(a0, ang)} class="knob-val" style={p.color ? { stroke: p.color } : {}} /> : null}
        <line x1={size / 2} y1={size / 2} x2={tip.x} y2={tip.y} class="knob-tip" />
      </svg>
      {p.label ? <span class="knob-label">{p.label}</span> : null}
    </div>
  );
}
function polar(cx: number, cy: number, r: number, deg: number) {
  const a = ((deg - 90) * Math.PI) / 180;
  return { x: cx + r * Math.cos(a), y: cy + r * Math.sin(a) };
}

// ---- fader (gain 0..1.25, FL-like taper) --------------------------------------------------------
const faderPos = (g: number) => Math.sqrt(clamp(g, 0, 1.25) / 1.25);
const faderGain = (p: number) => 1.25 * p * p;
export function Fader(p: { value: number; onChange: (v: number, final: boolean) => void; height?: number; def?: number; color?: string }) {
  const h = p.height ?? 120;
  const pos = faderPos(p.value);
  const acc = useRef(pos);
  acc.current = pos;
  const drag = useDrag(
    (dy, fine) => {
      acc.current = clamp(acc.current + dy / (fine ? h * 6 : h), 0, 1);
      p.onChange(faderGain(acc.current), false);
    },
    () => p.onChange(faderGain(acc.current), true),
  );
  return (
    <div
      class="fader"
      style={{ height: `${h}px`, touchAction: 'none' }}
      {...drag}
      onDblClick={() => p.onChange(p.def ?? 0.8, true)}
      onWheel={(e: WheelEvent) => {
        e.preventDefault();
        p.onChange(faderGain(clamp(pos + (e.deltaY < 0 ? 0.02 : -0.02), 0, 1)), true);
      }}
      role="slider"
      aria-valuenow={p.value}
    >
      <div class="fader-slot" />
      <div class="fader-unity" style={{ bottom: `${faderPos(1) * (h - 18)}px` }} />
      <div class="fader-thumb" style={{ bottom: `${pos * (h - 18)}px`, ...(p.color ? { borderTopColor: p.color } : {}) }} />
    </div>
  );
}

// ---- meter: peak (linear) shown on a dB scale -60..+6 ------------------------------------------
export const meterPct = (v: number) => (v <= 0.001 ? 0 : clamp(((20 * Math.log10(v) + 60) / 66) * 100, 0, 100));
export function Meter(p: { l: number; r?: number; height?: number; width?: number }) {
  const bar = (v: number) => (
    <div class="meter-bar" style={{ width: `${p.width ?? 5}px` }}>
      <div class={`meter-fill${v >= 1 ? ' clip' : ''}`} style={{ height: `${meterPct(v)}%` }} />
    </div>
  );
  return (
    <div class="meter" style={{ height: `${p.height ?? 120}px` }}>
      {bar(p.l)}
      {p.r !== undefined ? bar(p.r) : null}
    </div>
  );
}

// ---- numeric drag field (BPM, step counts) ------------------------------------------------------
export function DragNumber(p: {
  value: number;
  min: number;
  max: number;
  step?: number;
  decimals?: number;
  width?: number;
  onChange: (v: number, final: boolean) => void;
  title?: string;
  class?: string;
}) {
  const [edit, setEdit] = useState(false);
  const acc = useRef(p.value);
  acc.current = p.value;
  const step = p.step ?? 1;
  const drag = useDrag(
    (dy, fine) => {
      acc.current = clamp(acc.current + dy * step * (fine ? 0.1 : 1) * 0.5, p.min, p.max);
      p.onChange(Math.round(acc.current / (fine ? step / 10 : step)) * (fine ? step / 10 : step), false);
    },
    () => p.onChange(acc.current, true),
  );
  if (edit)
    return (
      <input
        class={`dragnum-edit ${p.class ?? ''}`}
        style={{ width: `${p.width ?? 56}px` }}
        autoFocus
        defaultValue={p.value.toFixed(p.decimals ?? 0)}
        onBlur={(e) => {
          setEdit(false);
          const v = parseFloat((e.target as HTMLInputElement).value);
          if (isFinite(v)) p.onChange(clamp(v, p.min, p.max), true);
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
          if (e.key === 'Escape') setEdit(false);
        }}
      />
    );
  return (
    <div class={`dragnum ${p.class ?? ''}`} style={{ width: `${p.width ?? 56}px`, touchAction: 'none' }} title={p.title} {...drag} onDblClick={() => setEdit(true)}>
      {p.value.toFixed(p.decimals ?? 0)}
    </div>
  );
}

// ---- buttons ----------------------------------------------------------------------------------
export function Btn(p: {
  onClick?: (e: MouseEvent) => void;
  onContextMenu?: (e: MouseEvent) => void;
  active?: boolean;
  title?: string;
  class?: string;
  disabled?: boolean;
  children: ComponentChildren;
  style?: JSX.CSSProperties;
}) {
  return (
    <button
      type="button"
      class={`btn${p.active ? ' on' : ''} ${p.class ?? ''}`}
      title={p.title}
      aria-label={p.title}
      aria-pressed={p.active}
      disabled={p.disabled}
      style={p.style}
      onClick={p.onClick}
      onContextMenu={p.onContextMenu}
    >
      {p.children}
    </button>
  );
}

export function Led(p: { on: boolean; onClick: () => void; title?: string; color?: string }) {
  return (
    <button
      type="button"
      class={`led${p.on ? ' on' : ''}`}
      title={p.title}
      aria-label={p.title}
      aria-pressed={p.on}
      style={p.on && p.color ? { background: p.color, boxShadow: `0 0 6px ${p.color}` } : {}}
      onClick={(e) => {
        e.stopPropagation();
        p.onClick();
      }}
    />
  );
}

// ---- menus (click / tap; position from the trigger) -------------------------------------------
export interface MenuItem {
  label: string;
  action?: () => void;
  disabled?: boolean;
  checked?: boolean;
  shortcut?: string;
  sep?: boolean;
  danger?: boolean;
}
let openMenu: ((m: { items: MenuItem[]; x: number; y: number } | null) => void) | null = null;
export function showMenu(items: MenuItem[], x: number, y: number) {
  openMenu?.({ items, x, y });
}
export function MenuHost() {
  const [m, setM] = useState<{ items: MenuItem[]; x: number; y: number } | null>(null);
  openMenu = setM;
  useEffect(() => {
    if (!m) return;
    const close = (e: PointerEvent) => {
      if (!(e.target as Element).closest?.('.menu')) setM(null);
    };
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && setM(null);
    window.addEventListener('pointerdown', close, true);
    window.addEventListener('keydown', esc);
    return () => {
      window.removeEventListener('pointerdown', close, true);
      window.removeEventListener('keydown', esc);
    };
  }, [m]);
  if (!m) return null;
  const x = Math.min(m.x, window.innerWidth - 230), y = Math.min(m.y, window.innerHeight - m.items.length * 30 - 10);
  return (
    <div class="menu" style={{ left: `${Math.max(4, x)}px`, top: `${Math.max(4, y)}px` }} role="menu">
      {m.items.map((it, i) =>
        it.sep ? (
          <div key={i} class="menu-sep" />
        ) : (
          <button
            key={i}
            type="button"
            role="menuitem"
            class={`menu-item${it.danger ? ' danger' : ''}`}
            disabled={it.disabled}
            onClick={() => {
              setM(null);
              it.action?.();
            }}
          >
            <span class="menu-check">{it.checked ? '✓' : ''}</span>
            <span class="menu-label">{it.label}</span>
            {it.shortcut ? <span class="menu-key">{it.shortcut}</span> : null}
          </button>
        ),
      )}
    </div>
  );
}
export const menuAt = (e: MouseEvent | PointerEvent, items: MenuItem[]) => {
  const r = (e.currentTarget as Element | null)?.getBoundingClientRect?.();
  showMenu(items, r && e.type === 'click' ? r.left : e.clientX, r && e.type === 'click' ? r.bottom + 2 : e.clientY);
};

// ---- modal dialog (desktop) / bottom sheet (phones) ---------------------------------------------
export function Modal(p: { title: string; onClose: () => void; children: ComponentChildren; wide?: boolean }) {
  useEffect(() => {
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && p.onClose();
    window.addEventListener('keydown', esc);
    return () => window.removeEventListener('keydown', esc);
  }, []);
  return (
    <div class="modal-back" onPointerDown={(e) => e.target === e.currentTarget && p.onClose()}>
      <div class={`modal${p.wide ? ' wide' : ''}`} role="dialog" aria-label={p.title}>
        <div class="modal-head">
          <span>{p.title}</span>
          <button type="button" class="btn icon" aria-label="Close" onClick={p.onClose}>
            <Icon name="x" />
          </button>
        </div>
        <div class="modal-body">{p.children}</div>
      </div>
    </div>
  );
}

// ---- icons ------------------------------------------------------------------------------------
const ICONS: Record<string, string> = {
  play: 'M7 4 L20 12 L7 20 Z',
  pause: 'M6 4 h4 v16 h-4 Z M14 4 h4 v16 h-4 Z',
  stop: 'M6 6 h12 v12 h-12 Z',
  rec: 'M12 5 a7 7 0 1 0 0.01 0 Z',
  metro: 'M9 3 h6 l4 18 h-14 Z M12 15 L17 6',
  loop: 'M4 12 a6 6 0 0 1 6-6 h8 l-3-3 M20 12 a6 6 0 0 1-6 6 h-8 l3 3',
  undo: 'M9 14 L4 9 L9 4 M4 9 h10 a6 6 0 0 1 0 12 h-4',
  redo: 'M15 14 L20 9 L15 4 M20 9 h-10 a6 6 0 0 0 0 12 h4',
  piano: 'M3 4 h18 v16 h-18 Z M8 4 v10 M13 4 v10 M18 4 v10 M8 14 v6 M13 14 v6',
  steps: 'M3 5 h4 v4 h-4 Z M10 5 h4 v4 h-4 Z M17 5 h4 v4 h-4 Z M3 15 h4 v4 h-4 Z M10 15 h4 v4 h-4 Z M17 15 h4 v4 h-4 Z',
  mixer: 'M6 3 v18 M12 3 v18 M18 3 v18 M3 14 h6 M9 8 h6 M15 16 h6',
  playlist: 'M3 5 h18 M3 12 h18 M3 19 h18 M6 3 v4 M14 10 v4 M9 17 v4',
  browser: 'M3 4 h7 l2 3 h9 v13 h-18 Z',
  plug: 'M9 2 v6 M15 2 v6 M6 8 h12 v4 a6 6 0 0 1-12 0 Z M12 18 v4',
  gear: 'M12 8 a4 4 0 1 0 0.01 0 Z M12 2 v3 M12 19 v3 M2 12 h3 M19 12 h3 M5 5 l2 2 M17 17 l2 2 M5 19 l2-2 M17 7 l2-2',
  plus: 'M12 5 v14 M5 12 h14',
  x: 'M6 6 L18 18 M18 6 L6 18',
  left: 'M15 5 L8 12 L15 19',
  right: 'M9 5 L16 12 L9 19',
  down: 'M5 9 L12 16 L19 9',
  pencil: 'M4 20 l4-1 L19 8 l-3-3 L5 16 Z',
  cursor: 'M5 3 L5 19 L9 15 L12 21 L15 20 L12 14 L18 14 Z',
  eraser: 'M3 17 L13 7 L19 13 L12 20 H6 Z M9 11 l6 6',
  scissors: 'M6 7 a2.5 2.5 0 1 0 0.01 0 M6 17 a2.5 2.5 0 1 0 0.01 0 M8 8 L20 18 M8 16 L20 6',
  magnet: 'M5 4 v8 a7 7 0 0 0 14 0 v-8 h-4 v8 a3 3 0 0 1-6 0 v-8 Z',
  zoomin: 'M10 4 a6 6 0 1 0 0.01 0 Z M15 15 l6 6 M10 7 v6 M7 10 h6',
  zoomout: 'M10 4 a6 6 0 1 0 0.01 0 Z M15 15 l6 6 M7 10 h6',
  menu: 'M4 6 h16 M4 12 h16 M4 18 h16',
  kbd: 'M2 6 h20 v12 h-20 Z M6 10 h1 M10 10 h1 M14 10 h1 M18 10 h1 M7 14 h10',
  song: 'M9 18 V5 l11-2 v13 M9 18 a3 3 0 1 1-0.01 0 M20 16 a3 3 0 1 1-0.01 0',
  more: 'M5 12 h0.01 M12 12 h0.01 M19 12 h0.01',
  wave: 'M2 12 q3-8 6 0 t6 0 t6 0',
  drum: 'M4 9 a8 3 0 1 0 16 0 a8 3 0 1 0-16 0 v7 a8 3 0 0 0 16 0 v-7',
  slide: 'M3 18 L9 18 L15 6 L21 6',
  follow: 'M4 4 v16 M8 12 h12 M16 8 l4 4 l-4 4',
  save: 'M5 3 h11 l3 3 v15 h-14 Z M8 3 v5 h7 v-5 M8 14 h8 v7 h-8 Z',
  folder: 'M3 6 h6 l2 2 h10 v11 h-18 Z',
  import: 'M12 3 v11 M7 9 l5 5 l5 -5 M4 16 v4 h16 v-4',
  dup: 'M8 8 h12 v12 h-12 Z M4 4 h12 v4 M4 4 v12 h4',
  tap: 'M12 3 v8 M8 7 l4 4 l4-4 M5 15 h14 v5 h-14 Z',
  power: 'M12 3 v8 M7 6 a8 8 0 1 0 10 0',
};
export function Icon(p: { name: string; size?: number; fill?: boolean }) {
  const filled = p.fill ?? ['play', 'pause', 'stop', 'rec'].includes(p.name);
  return (
    <svg class="ico" width={p.size ?? 16} height={p.size ?? 16} viewBox="0 0 24 24" aria-hidden="true">
      <path
        d={ICONS[p.name] ?? ''}
        fill={filled ? 'currentColor' : 'none'}
        stroke={filled ? 'none' : 'currentColor'}
        stroke-width="2"
        stroke-linecap="round"
        stroke-linejoin="round"
      />
    </svg>
  );
}
