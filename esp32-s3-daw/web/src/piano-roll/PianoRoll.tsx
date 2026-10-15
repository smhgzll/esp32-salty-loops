// SPDX-License-Identifier: GPL-3.0-or-later
// Piano roll: canvas editor for one channel's notes in the current pattern.
//   draw tool   click empty space = new note (drag sets its length); drag a note = move; drag its
//               right edge = resize; Ctrl/Alt-drag = duplicate; right-click = delete
//   select tool drag a rectangle; then move / resize / delete / copy the selection
//   erase tool  click or sweep to delete;   slide tool: draws slide notes (glide into the next key)
//   bottom lane velocity / pan / fine pitch / release / probability: drag to paint values
//   touch       one finger = tool, two fingers = pan + pinch zoom, long press = note menu
//   keys        Del, Ctrl+A/C/X/V/D, arrows (move / transpose, Shift = octave), Q quantize
import { useEffect, useRef, useState } from 'preact/hooks';

import { useCanvasLive } from '../app/canvas';

import * as A from '../app/actions';
import { barTicks, catalog, live, P, proj, ui } from '../app/state';
import { useStore } from '../app/store';
import type { Note, PluginInfo } from '../app/types';
import { toNote } from '../app/types';
import { clamp, isBlack, noteName, rgba, shade } from '../app/util';
import { Btn, Icon, Knob, Modal, menuAt, showMenu } from '../ui/controls';
import type { MenuItem } from '../ui/controls';
import { togglePanel } from '../transport/TopBar';
import { importMidi, isMidiFile } from '../midi/MidiImport';
import * as M from './model';

type Tool = 'draw' | 'select' | 'erase' | 'slide';
type Lane = 'vel' | 'pan' | 'pitch' | 'rel' | 'prob';
const LANES: { id: Lane; label: string; cap?: string }[] = [
  { id: 'vel', label: 'Velocity' },
  { id: 'pan', label: 'Pan', cap: 'perNotePan' },
  { id: 'pitch', label: 'Fine pitch', cap: 'perNotePitch' },
  { id: 'rel', label: 'Release', cap: 'releaseVelocity' },
  { id: 'prob', label: 'Probability' },
];
let clipboard: Note[] = [];

interface View {
  x0: number; // tick at the grid's left edge
  top: number; // key value at the grid's top edge
  ppt: number; // pixels per tick
  kh: number; // pixels per key row
}
type Gesture =
  | { kind: 'create' | 'move' | 'resize' | 'dup'; id: number; ids: Set<number>; t0: number; k0: number; before: Note[]; base: Note[]; moved: boolean; px: number; py: number }
  | { kind: 'rect'; t0: number; k0: number; t1: number; k1: number; add: boolean }
  | { kind: 'erase'; before: Note[] }
  | { kind: 'lane'; before: Note[] }
  | { kind: 'pan'; x: number; y: number; v: View }
  | { kind: 'key'; key: number }
  | { kind: 'seek' };

export function PianoRoll(p: { mobile?: boolean }) {
  const project = useStore(proj, (s) => s.project);
  const chanId = useStore(ui, (s) => s.pianoChannel);
  const follow = useStore(ui, (s) => s.follow);
  const rollFit = useStore(ui, (s) => s.rollFit);
  const plugins = useStore(catalog, (s) => s.plugins);
  const wrap = useRef<HTMLDivElement>(null);
  const cv = useRef<HTMLCanvasElement>(null);
  const ov = useRef<HTMLCanvasElement>(null);
  const [tool, setTool] = useState<Tool>('draw');
  const [snapI, setSnapI] = useState(3);
  const [lane, setLane] = useState<Lane>('vel');
  const [ghosts, setGhosts] = useState(true);
  const [props, setProps] = useState<Note | null>(null);
  const [, redraw] = useState(0);
  const st = useRef({
    view: { x0: 0, top: 84, ppt: p.mobile ? 0.09 : 0.11, kh: p.mobile ? 18 : 14 } as View,
    notes: [] as Note[],
    sel: new Set<number>(),
    lastLen: 240,
    g: null as Gesture | null,
    pointers: new Map<number, { x: number; y: number }>(),
    pinch: null as null | { d0x: number; d0y: number; cx: number; cy: number; v: View },
    lp: null as ReturnType<typeof setTimeout> | null,
    lastTap: 0,
    lastTapId: -1,
    held: -1,
    size: { w: 0, h: 0 },
    centred: -1,
  });
  const S = st.current;

  const pat = project?.patterns.find((x) => x.id === project.meta.currentPattern) ?? project?.patterns[0];
  const chan = project?.channels.find((c) => c.id === chanId) ?? project?.channels[0];
  const info: PluginInfo | undefined = chan ? plugins.find((x) => x.id === chan.plugin.id) : undefined;
  const KW = p.mobile ? 44 : 62, RH = 22, LH = p.mobile ? 64 : 86;
  const snapTicks = () => {
    const s = M.SNAPS[snapI].ticks;
    return s < 0 ? barTicks() : s;
  };

  // the project's notes, unless a gesture is editing them
  if (!S.g && pat && chan) {
    const fresh = (pat.seqs.find((s) => s.channel === chan.id)?.notes ?? []).map(toNote);
    S.notes = fresh;
    for (const id of [...S.sel]) if (!fresh.some((n) => n.id === id)) S.sel.delete(id);
  }
  // after an import: show the notes again (their keys, from the start)
  const lastFit = useRef(rollFit);
  if (lastFit.current !== rollFit) {
    lastFit.current = rollFit;
    S.centred = -1;
    S.view.x0 = 0;
  }
  // first view of a channel: centre its notes (once the editor's height is known)
  const centre = () => {
    if (!chan || S.centred === chan.id || S.size.h <= 0) return;
    S.centred = chan.id;
    // the notes' range, or the channel's root key when it has none yet
    const keys = S.notes.length ? S.notes.map((n) => n.key) : [chan.root || 60];
    const half = Math.max(4, (S.size.h - LH - RH) / S.view.kh / 2);
    S.view.top = clamp(Math.round((Math.min(...keys) + Math.max(...keys)) / 2 + half), 20, 127);
  };
  centre();

  // ---- geometry ------------------------------------------------------------------------------
  const tickAt = (x: number) => S.view.x0 + (x - KW) / S.view.ppt;
  const xOf = (t: number) => KW + (t - S.view.x0) * S.view.ppt;
  const keyAt = (y: number) => Math.floor(S.view.top - (y - RH) / S.view.kh);
  const yOf = (k: number) => RH + (S.view.top - (k + 1)) * S.view.kh;
  const gridBottom = () => S.size.h - LH;
  const noteAt = (x: number, y: number): [Note | null, 'body' | 'edge'] => {
    const t = tickAt(x), k = keyAt(y);
    for (let i = S.notes.length - 1; i >= 0; i--) {
      const n = S.notes[i];
      if (n.key === k && t >= n.start && t < n.start + n.len) {
        const xe = xOf(n.start + n.len);
        const w = (n.len) * S.view.ppt;
        return [n, xe - x < Math.min(8, Math.max(3, w * 0.3)) ? 'edge' : 'body'];
      }
    }
    return [null, 'body'];
  };

  // ---- drawing -------------------------------------------------------------------------------
  const draw = () => {
    const c = cv.current;
    if (!c || !project || !pat) return;
    const dpr = window.devicePixelRatio || 1;
    const { w, h } = S.size;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
      c.style.width = `${w}px`;
      c.style.height = `${h}px`;
      const o = ov.current!;
      o.width = c.width;
      o.height = c.height;
      o.style.width = `${w}px`;
      o.style.height = `${h}px`;
    }
    const g = c.getContext('2d')!;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    const v = S.view, gb = gridBottom();
    const bar = barTicks(), beat = bar / project.meta.timeSig[0], snap = snapTicks();
    const color = chan?.color ?? '#ff9a3c';
    g.fillStyle = '#1b1f23';
    g.fillRect(0, 0, w, h);
    // key rows
    const kLo = Math.max(0, keyAt(gb) - 1), kHi = Math.min(127, Math.ceil(v.top));
    for (let k = kLo; k <= kHi; k++) {
      const y = yOf(k);
      g.fillStyle = isBlack(k) ? '#1d2125' : '#262b30';
      g.fillRect(KW, y, w - KW, v.kh);
      g.fillStyle = k % 12 === 0 ? '#14171a' : '#202428';
      g.fillRect(KW, y + v.kh - 1, w - KW, 1);
    }
    // time lines
    const t0 = Math.max(0, v.x0), t1 = tickAt(w);
    const fine = snap > 1 && snap * v.ppt >= 6 ? snap : beat * v.ppt >= 8 ? beat : bar;
    for (let t = Math.floor(t0 / fine) * fine; t <= t1; t += fine) {
      const x = Math.round(xOf(t)) + 0.5;
      g.fillStyle = t % bar === 0 ? '#0e1012' : t % beat === 0 ? '#171a1d' : '#1e2226';
      g.fillRect(x - 0.5, RH, t % bar === 0 ? 1.5 : 1, gb - RH);
    }
    // beyond the pattern's end
    const end = pat.effLength || bar;
    if (xOf(end) < w) {
      g.fillStyle = 'rgba(0,0,0,0.35)';
      g.fillRect(Math.max(KW, xOf(end)), RH, w, gb - RH);
      g.fillStyle = '#ff9a3c';
      g.fillRect(xOf(end), RH, 2, gb - RH);
    }
    g.save();
    g.beginPath();
    g.rect(KW, RH, w - KW, gb - RH);
    g.clip();
    // ghost notes of the other channels
    if (ghosts)
      for (const s of pat.seqs) {
        if (s.channel === chan?.id || !s.pianoRoll) continue;
        const gc = project.channels.find((x) => x.id === s.channel)?.color ?? '#888';
        g.fillStyle = rgba(gc, 0.18);
        for (const a of s.notes) {
          const x = xOf(a[1]);
          if (x > w || x + a[2] * v.ppt < KW) continue;
          g.fillRect(x, yOf(a[3]) + 1, Math.max(2, a[2] * v.ppt - 1), v.kh - 2);
        }
      }
    // notes
    g.font = `${Math.min(11, v.kh - 3)}px system-ui`;
    g.textBaseline = 'middle';
    for (const n of S.notes) {
      const x = xOf(n.start), y = yOf(n.key), wd = Math.max(3, n.len * v.ppt - 1);
      if (x > w || x + wd < KW || y > gb || y + v.kh < RH) continue;
      const sel = S.sel.has(n.id);
      const base = shade(color, -0.45 + 0.45 * n.vel);
      g.fillStyle = sel ? shade(color, 0.35) : base;
      roundRect(g, x, y + 1, wd, v.kh - 2, 2);
      g.fill();
      g.fillStyle = 'rgba(255,255,255,0.18)';
      g.fillRect(x + 1, y + 1, wd - 2, 2);
      g.strokeStyle = sel ? '#ffffff' : 'rgba(0,0,0,0.75)';
      g.lineWidth = 1;
      roundRect(g, x + 0.5, y + 1.5, wd - 1, v.kh - 3, 2);
      g.stroke();
      g.fillStyle = 'rgba(0,0,0,0.45)';
      if (wd > 10) g.fillRect(x + wd - 4, y + 3, 2, v.kh - 6); // resize grip
      if (n.slide) {
        g.strokeStyle = '#fff';
        g.beginPath();
        g.moveTo(x + 2, y + v.kh - 3);
        g.lineTo(x + Math.min(12, wd - 2), y + 3);
        g.stroke();
      }
      if (wd > 28 && v.kh >= 10) {
        g.fillStyle = 'rgba(0,0,0,0.75)';
        g.fillText(noteName(n.key), x + (n.slide ? 14 : 4), y + v.kh / 2);
      }
      if (n.prob < 1 && wd > 14) {
        g.fillStyle = '#000';
        g.fillText('?', x + wd - 10, y + v.kh / 2);
      }
    }
    // selection rectangle
    if (S.g?.kind === 'rect') {
      const r = S.g;
      const xa = xOf(Math.min(r.t0, r.t1)), xb = xOf(Math.max(r.t0, r.t1));
      const ya = yOf(Math.max(r.k0, r.k1)), yb = yOf(Math.min(r.k0, r.k1)) + v.kh;
      g.fillStyle = 'rgba(255,154,60,0.15)';
      g.fillRect(xa, ya, xb - xa, yb - ya);
      g.strokeStyle = '#ff9a3c';
      g.strokeRect(xa + 0.5, ya + 0.5, xb - xa, yb - ya);
    }
    g.restore();
    // keyboard
    for (let k = kLo; k <= kHi; k++) {
      const y = yOf(k);
      const down = S.held === k;
      g.fillStyle = down ? '#ff9a3c' : isBlack(k) ? '#16181b' : '#e9ecef';
      g.fillRect(0, y, isBlack(k) ? KW * 0.62 : KW, v.kh);
      if (isBlack(k)) {
        g.fillStyle = down ? '#ff9a3c' : '#e9ecef';
        g.fillRect(KW * 0.62, y, KW * 0.38, v.kh);
      }
      g.fillStyle = '#7d858c';
      g.fillRect(0, y + v.kh - 1, KW, 1);
      if (k % 12 === 0 && v.kh >= 9) {
        g.fillStyle = '#333';
        g.font = '9px system-ui';
        g.fillText(noteName(k), KW - 24, y + v.kh / 2);
      }
      const kn = info?.keyNames?.[String(k)];
      if (kn && v.kh >= 10) {
        g.fillStyle = isBlack(k) ? '#ccc' : '#333';
        g.font = '9px system-ui';
        g.fillText(kn.slice(0, 7), 2, y + v.kh / 2);
      }
    }
    g.fillStyle = '#0b0d0f';
    g.fillRect(KW - 1, RH, 1, gb - RH);
    // ruler
    g.fillStyle = '#2a2f35';
    g.fillRect(0, 0, w, RH);
    g.fillStyle = '#0b0d0f';
    g.fillRect(0, RH - 1, w, 1);
    g.font = '10px system-ui';
    g.fillStyle = '#c0c6cc';
    const step = bar * v.ppt < 40 ? bar * 4 : bar;
    for (let t = Math.floor(t0 / bar) * bar; t <= t1; t += step) {
      const x = xOf(t);
      if (x < KW) continue;
      g.fillRect(x, 8, 1, RH - 8);
      g.fillText(String(t / bar + 1), x + 3, 10);
    }
    for (let t = Math.floor(t0 / beat) * beat; t <= t1; t += beat) {
      const x = xOf(t);
      if (x >= KW && t % bar) g.fillRect(x, RH - 6, 1, 5);
    }
    // the expression lane
    const ly = gb;
    g.fillStyle = '#16191c';
    g.fillRect(0, ly, w, LH);
    g.fillStyle = '#0b0d0f';
    g.fillRect(0, ly, w, 2);
    g.fillStyle = '#7d858c';
    g.font = '10px system-ui';
    g.fillText(LANES.find((l) => l.id === lane)!.label, 4, ly + 12);
    const laneOk = laneAvailable(lane);
    if (!laneOk) {
      g.fillStyle = '#6b757f';
      g.fillText(`${info?.name ?? 'This instrument'} has no independent per-note ${LANES.find((l) => l.id === lane)!.label.toLowerCase()}`, KW + 8, ly + LH / 2);
    } else {
      const bip = lane === 'pan' || lane === 'pitch';
      const mid = ly + 4 + (LH - 8) / 2;
      if (bip) {
        g.fillStyle = '#2a2f35';
        g.fillRect(KW, mid, w - KW, 1);
      }
      for (const n of S.notes) {
        const x = xOf(n.start);
        if (x < KW - 4 || x > w) continue;
        const val = laneValue(n, lane);
        const sel = S.sel.has(n.id);
        g.fillStyle = sel ? shade(color, 0.4) : color;
        const hh = (LH - 8) * (bip ? Math.abs(val) / 2 : val);
        const y = bip ? (val >= 0 ? mid - hh : mid) : ly + LH - 4 - hh;
        g.fillRect(x, y, 3, Math.max(1, hh));
        g.beginPath();
        g.arc(x + 1.5, bip ? (val >= 0 ? y : y + hh) : y, 3, 0, Math.PI * 2);
        g.fill();
      }
    }
    drawPlayhead();
  };

  const laneAvailable = (l: Lane) => {
    const cap = LANES.find((x) => x.id === l)!.cap;
    return !cap || !!info?.capabilities[cap];
  };
  const laneValue = (n: Note, l: Lane) => (l === 'vel' ? n.vel : l === 'pan' ? n.pan : l === 'pitch' ? n.pitch : l === 'rel' ? n.rel : n.prob);
  const setLaneValue = (n: Note, l: Lane, v: number): Note =>
    l === 'vel' ? { ...n, vel: clamp(v, 0.02, 1) } : l === 'pan' ? { ...n, pan: clamp(v, -1, 1) } : l === 'pitch' ? { ...n, pitch: clamp(v, -1, 1) } : l === 'rel' ? { ...n, rel: clamp(v, 0, 1) } : { ...n, prob: clamp(v, 0, 1) };

  const drawPlayhead = () => {
    const o = ov.current;
    if (!o || S.size.w <= KW || S.size.h <= RH) return;
    const g = o.getContext('2d')!;
    const dpr = window.devicePixelRatio || 1;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, S.size.w, S.size.h);
    const s = live.state.status;
    if (!s || !pat || s.mode !== 'pattern' || s.pattern !== pat.id || s.play === 0) return;
    const x = xOf(s.tick);
    if (follow && s.play === 1 && (x > S.size.w - 40 || x < KW)) {
      const x0 = Math.max(0, s.tick - 20 / S.view.ppt);
      // Before layout, or in a narrow grid, following may leave the head outside the
      // margin. Only redraw if scrolling actually changes the view.
      if (x0 !== S.view.x0) {
        S.view.x0 = x0;
        draw();
        return;
      }
    }
    if (x < KW || x > S.size.w) return;
    g.fillStyle = '#7de08a';
    g.fillRect(x, 0, 1.5, gridBottom());
    g.beginPath();
    g.moveTo(x - 5, 0);
    g.lineTo(x + 6, 0);
    g.lineTo(x + 0.5, 7);
    g.fill();
  };

  // ---- size, live playhead ----------------------------------------------------------------------
  useCanvasLive(wrap, (el) => {
      const h0 = S.size.h;
      S.size = { w: el.clientWidth, h: el.clientHeight };
      // a resize (rotating a phone, dragging a splitter) keeps the key in the middle of the grid there
      if (h0 > 0 && S.size.h !== h0) S.view.top = clamp(S.view.top + (S.size.h - h0) / S.view.kh / 2, 8, 127);
      centre();
      draw();
  }, drawPlayhead);
  useEffect(draw);

  // ---- editing ------------------------------------------------------------------------------------
  const commit = (label: string, before: Note[]) => {
    if (!pat || !chan) return;
    A.setNotes(pat.id, chan.id, S.notes, label, before);
  };
  const apply = (label: string, fn: (ns: Note[]) => Note[]) => {
    const before = S.notes;
    S.notes = fn(S.notes);
    commit(label, before);
    redraw((x) => x + 1);
  };
  const selIds = () => (S.sel.size ? S.sel : new Set(S.notes.map((n) => n.id)));

  const opsMenu = (): MenuItem[] => [
    { label: 'Quantize starts', shortcut: 'Q', action: () => apply('Quantize', (ns) => M.quantize(ns, S.sel, snapTicks())) },
    { label: 'Quantize starts + lengths', action: () => apply('Quantize', (ns) => M.quantize(ns, S.sel, snapTicks(), true)) },
    { label: 'Transpose +1', shortcut: '↑', action: () => apply('Transpose', (ns) => M.transpose(ns, S.sel, 1)) },
    { label: 'Transpose -1', shortcut: '↓', action: () => apply('Transpose', (ns) => M.transpose(ns, S.sel, -1)) },
    { label: 'Octave up', shortcut: 'Shift+↑', action: () => apply('Octave up', (ns) => M.transpose(ns, S.sel, 12)) },
    { label: 'Octave down', shortcut: 'Shift+↓', action: () => apply('Octave down', (ns) => M.transpose(ns, S.sel, -12)) },
    { label: 'Legato', action: () => apply('Legato', (ns) => M.legato(ns, S.sel)) },
    { label: 'Chop to grid', action: () => apply('Chop', (ns) => M.chop(ns, S.sel, snapTicks())) },
    { label: 'Randomize velocity', action: () => apply('Random velocity', (ns) => M.randomizeVelocity(ns, S.sel, 0.2)) },
    { label: 'Double length', action: () => apply('Length x2', (ns) => ns.map((n) => (selIds().has(n.id) ? { ...n, len: n.len * 2 } : n))) },
    { label: 'Halve length', action: () => apply('Length /2', (ns) => ns.map((n) => (selIds().has(n.id) ? { ...n, len: Math.max(30, Math.round(n.len / 2)) } : n))) },
    { sep: true, label: '' },
    { label: 'Select all', shortcut: 'Ctrl+A', action: () => { S.sel = new Set(S.notes.map((n) => n.id)); redraw((x) => x + 1); } },
    { label: 'Copy', shortcut: 'Ctrl+C', action: copy },
    { label: 'Cut', shortcut: 'Ctrl+X', action: () => { copy(); apply('Cut', (ns) => M.deleteNotes(ns, S.sel)); } },
    { label: 'Paste', shortcut: 'Ctrl+V', action: () => pasteAt(S.view.x0 + 20 / S.view.ppt) },
    { label: 'Duplicate', shortcut: 'Ctrl+D', action: dupSel },
    { label: 'Delete selection', shortcut: 'Del', action: () => apply('Delete notes', (ns) => M.deleteNotes(ns, S.sel)) },
    { sep: true, label: '' },
    { label: 'Ghost notes', checked: ghosts, action: () => setGhosts(!ghosts) },
    { label: 'Follow playhead', checked: follow, action: () => ui.set({ follow: !follow }) },
    { sep: true, label: '' },
    { label: 'Import MIDI file…', action: () => importMidi('piano') },
  ];
  function copy() {
    clipboard = S.notes.filter((n) => S.sel.has(n.id));
  }
  function pasteAt(t: number) {
    const before = S.notes;
    const [ns, ids] = M.paste(S.notes, clipboard, Math.max(0, M.snapDown(t, snapTicks())));
    S.notes = ns;
    S.sel = ids;
    commit('Paste', before);
    redraw((x) => x + 1);
  }
  function dupSel() {
    if (!S.sel.size) return;
    const [a, b] = M.span(S.notes, S.sel);
    const bar = barTicks();
    const off = Math.max(snapTicks(), Math.ceil((b - a) / bar) * bar);
    const before = S.notes;
    const [ns, ids] = M.duplicate(S.notes, S.sel, off);
    S.notes = ns;
    S.sel = ids;
    commit('Duplicate', before);
    redraw((x) => x + 1);
  }

  const noteMenu = (n: Note, x: number, y: number) =>
    showMenu(
      [
        { label: 'Properties…', action: () => setProps(n) },
        { label: n.slide ? 'Normal note' : 'Slide note', disabled: !info?.capabilities.slide, action: () => apply('Slide', (ns) => ns.map((m) => (m.id === n.id ? { ...m, slide: !m.slide } : m))) },
        { label: 'Duplicate', action: () => { S.sel = new Set([n.id]); dupSel(); } },
        { label: 'Delete', danger: true, action: () => apply('Delete note', (ns) => ns.filter((m) => m.id !== n.id)) },
      ],
      x,
      y,
    );

  // ---- pointer input ---------------------------------------------------------------------------------
  const local = (e: PointerEvent) => {
    const r = cv.current!.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };
  const cancelLongPress = () => {
    if (S.lp) clearTimeout(S.lp);
    S.lp = null;
  };
  const releaseKey = () => {
    if (S.held >= 0 && chan) A.noteOff(chan.id, S.held);
    S.held = -1;
  };

  const onDown = (e: PointerEvent) => {
    wrap.current?.focus();
    const { x, y } = local(e);
    S.pointers.set(e.pointerId, { x, y });
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    if (S.pointers.size === 2) {
      // two fingers: abandon the single-finger gesture, start pan / pinch
      cancelLongPress();
      if (S.g && 'before' in S.g) S.notes = S.g.before;
      releaseKey();
      const [a, b] = [...S.pointers.values()];
      S.g = null;
      S.pinch = { d0x: Math.abs(a.x - b.x), d0y: Math.abs(a.y - b.y), cx: (a.x + b.x) / 2, cy: (a.y + b.y) / 2, v: { ...S.view } };
      draw();
      return;
    }
    if (S.pointers.size > 2 || !chan || !pat) return;
    const t = tickAt(x), k = keyAt(y);
    const snap = snapTicks();
    if (e.button === 1 || (e.button === 0 && e.altKey && tool === 'select')) {
      S.g = { kind: 'pan', x, y, v: { ...S.view } };
      return;
    }
    if (y < RH) {
      S.g = { kind: 'seek' };
      A.seek(Math.max(0, M.snapDown(t, snap)));
      return;
    }
    if (x < KW && y < gridBottom()) {
      S.g = { kind: 'key', key: k };
      S.held = k;
      A.noteOn(chan.id, k, clamp((x / KW) * 1.2, 0.3, 1));
      draw();
      return;
    }
    if (y >= gridBottom()) {
      if (!laneAvailable(lane)) return;
      S.g = { kind: 'lane', before: S.notes };
      laneEdit(x, y);
      return;
    }
    const [hit, part] = noteAt(x, y);
    if (e.button === 2 || tool === 'erase') {
      S.g = { kind: 'erase', before: S.notes };
      if (hit) S.notes = S.notes.filter((n) => n.id !== hit.id);
      draw();
      return;
    }
    // double tap / double click on a note: properties
    const now = Date.now();
    if (hit && hit.id === S.lastTapId && now - S.lastTap < 320) {
      S.lastTap = 0;
      setProps(hit);
      return;
    }
    S.lastTap = now;
    S.lastTapId = hit ? hit.id : -1;
    if (hit) {
      if (!S.sel.has(hit.id)) S.sel = e.shiftKey ? new Set([...S.sel, hit.id]) : new Set([hit.id]);
      const dup = e.ctrlKey || e.metaKey;
      S.g = { kind: part === 'edge' ? 'resize' : dup ? 'dup' : 'move', id: hit.id, ids: new Set(S.sel), t0: t, k0: k, before: S.notes, base: S.notes, moved: false, px: x, py: y };
      if (part !== 'edge') A.previewNote(chan.id, hit.key, 180);
      if (e.pointerType === 'touch')
        S.lp = setTimeout(() => {
          S.lp = null;
          if (S.g && 'moved' in S.g && !S.g.moved) {
            S.notes = S.g.before;
            S.g = null;
            noteMenu(hit, e.clientX, e.clientY);
          }
        }, 520);
      draw();
      return;
    }
    if (tool === 'select' || e.shiftKey) {
      S.g = { kind: 'rect', t0: t, k0: k, t1: t, k1: k, add: e.shiftKey };
      if (!e.shiftKey) S.sel = new Set();
      draw();
      return;
    }
    // draw / slide: a new note
    const id = M.nextId(S.notes);
    const start = Math.max(0, M.snapDown(t, snap));
    const n = M.newNote(id, start, Math.max(snap > 1 ? snap : 60, S.lastLen), clamp(k, 0, 127));
    if (tool === 'slide') n.slide = true;
    const before = S.notes;
    S.notes = [...S.notes, n];
    S.sel = new Set([id]);
    S.g = { kind: 'create', id, ids: new Set([id]), t0: start, k0: k, before, base: S.notes, moved: false, px: x, py: y };
    A.previewNote(chan.id, n.key, 180);
    draw();
  };

  const laneEdit = (x: number, y: number) => {
    const ly = gridBottom();
    const bip = lane === 'pan' || lane === 'pitch';
    const f = clamp(1 - (y - ly - 4) / (LH - 8), 0, 1);
    const val = bip ? f * 2 - 1 : f;
    const t = tickAt(x), tol = 6 / S.view.ppt;
    const only = S.sel.size > 0;
    S.notes = S.notes.map((n) => (Math.abs(n.start - t) <= tol && (!only || S.sel.has(n.id)) ? setLaneValue(n, lane, val) : n));
    draw();
  };

  const onMove = (e: PointerEvent) => {
    const pt = local(e);
    if (S.pointers.has(e.pointerId)) S.pointers.set(e.pointerId, pt);
    if (S.pinch && S.pointers.size >= 2) {
      const [a, b] = [...S.pointers.values()];
      const pz = S.pinch;
      const dx = Math.abs(a.x - b.x), dy = Math.abs(a.y - b.y), cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
      const v = { ...pz.v };
      if (pz.d0x > 40) v.ppt = clamp(pz.v.ppt * (dx / pz.d0x), 0.01, 1.5);
      if (pz.d0y > 40) v.kh = clamp(pz.v.kh * (dy / pz.d0y), 6, 40);
      // keep the tick / key under the pinch centre in place, then pan by the centre's movement
      const tC = pz.v.x0 + (pz.cx - KW) / pz.v.ppt;
      const kC = pz.v.top - (pz.cy - RH) / pz.v.kh;
      v.x0 = Math.max(0, tC - (cx - KW) / v.ppt);
      v.top = clamp(kC + (cy - RH) / v.kh, 10, 128);
      S.view = v;
      draw();
      return;
    }
    const g = S.g;
    if (!g) return;
    const { x, y } = pt;
    const snap = snapTicks();
    switch (g.kind) {
      case 'pan':
        S.view.x0 = Math.max(0, g.v.x0 - (x - g.x) / S.view.ppt);
        S.view.top = clamp(g.v.top + (y - g.y) / S.view.kh, 10, 128);
        break;
      case 'key': {
        const k = keyAt(y);
        if (k !== g.key && chan) {
          A.noteOff(chan.id, g.key);
          A.noteOn(chan.id, k, 0.8);
          g.key = k;
          S.held = k;
        }
        break;
      }
      case 'seek':
        return;
      case 'lane':
        laneEdit(x, y);
        return;
      case 'erase': {
        const [hit] = noteAt(x, y);
        if (hit) S.notes = S.notes.filter((n) => n.id !== hit.id);
        break;
      }
      case 'rect':
        g.t1 = tickAt(x);
        g.k1 = keyAt(y);
        {
          const t0 = Math.min(g.t0, g.t1), t1 = Math.max(g.t0, g.t1), k0 = Math.min(g.k0, g.k1), k1 = Math.max(g.k0, g.k1);
          const ids = new Set(S.notes.filter((n) => M.inRect(n, t0, t1, k0, k1)).map((n) => n.id));
          S.sel = ids;
        }
        break;
      default: {
        if (!g.moved && Math.abs(x - g.px) < 3 && Math.abs(y - g.py) < 3) return;
        g.moved = true;
        cancelLongPress();
        const t = tickAt(x), k = keyAt(y);
        if (g.kind === 'create') {
          const n0 = g.base.find((n) => n.id === g.id)!;
          const len = Math.max(snap > 1 ? snap : 30, M.snapNear(t - n0.start, snap));
          S.notes = g.base.map((n) => (n.id === g.id ? { ...n, len, key: clamp(k, 0, 127) } : n));
          if (k !== n0.key && chan) A.previewNote(chan.id, clamp(k, 0, 127), 120);
          S.lastLen = len;
        } else if (g.kind === 'resize') {
          const dl = M.snapNear(t - g.t0, snap);
          S.notes = M.resizeNotes(g.base, g.ids, dl, snap > 1 ? snap : 30);
          const n = S.notes.find((m) => m.id === g.id);
          if (n) S.lastLen = n.len;
        } else {
          const anchor = g.base.find((n) => n.id === g.id)!;
          const dt = M.snapNear(anchor.start + (t - g.t0), snap) - anchor.start;
          const dk = k - g.k0;
          let base = g.base, ids = g.ids;
          if (g.kind === 'dup') {
            const [ns, nid] = M.duplicate(g.base, g.ids, 0);
            base = ns;
            ids = nid;
          }
          const prevKey = S.notes.find((n) => n.id === g.id)?.key;
          S.notes = M.moveNotes(base, ids, dt, dk);
          if (g.kind === 'dup') S.sel = ids;
          if (dk && anchor.key + dk !== prevKey && chan) A.previewNote(chan.id, clamp(anchor.key + dk, 0, 127), 100);
        }
      }
    }
    draw();
  };

  const onUp = (e: PointerEvent) => {
    S.pointers.delete(e.pointerId);
    cancelLongPress();
    if (S.pinch) {
      if (S.pointers.size < 2) S.pinch = null;
      return;
    }
    const g = S.g;
    S.g = null;
    if (!g) return;
    switch (g.kind) {
      case 'key':
        releaseKey();
        break;
      case 'create':
        commit('Add note', g.before);
        break;
      case 'move':
      case 'dup':
      case 'resize':
        if (g.moved) commit(g.kind === 'resize' ? 'Resize notes' : g.kind === 'dup' ? 'Duplicate notes' : 'Move notes', g.before);
        break;
      case 'erase':
        if (S.notes.length !== g.before.length) commit('Delete notes', g.before);
        break;
      case 'lane':
        commit(`Edit ${LANES.find((l) => l.id === lane)!.label.toLowerCase()}`, g.before);
        break;
    }
    draw();
  };

  const onWheel = (e: WheelEvent) => {
    e.preventDefault();
    const { x } = { x: e.clientX - cv.current!.getBoundingClientRect().left };
    if (e.ctrlKey || e.metaKey) {
      const t = tickAt(x);
      S.view.ppt = clamp(S.view.ppt * (e.deltaY < 0 ? 1.15 : 1 / 1.15), 0.01, 1.5);
      S.view.x0 = Math.max(0, t - (x - KW) / S.view.ppt);
    } else if (e.altKey) {
      S.view.kh = clamp(S.view.kh * (e.deltaY < 0 ? 1.1 : 1 / 1.1), 6, 40);
    } else if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
      S.view.x0 = Math.max(0, S.view.x0 + (e.deltaX || e.deltaY) / S.view.ppt);
    } else {
      S.view.top = clamp(S.view.top - e.deltaY / S.view.kh / 2, 10, 128);
    }
    draw();
  };

  const onKey = (e: KeyboardEvent) => {
    const ctrl = e.ctrlKey || e.metaKey;
    const snap = snapTicks();
    if (e.key === 'Delete' || e.key === 'Backspace') apply('Delete notes', (ns) => M.deleteNotes(ns, S.sel));
    else if (ctrl && e.key === 'a') {
      S.sel = new Set(S.notes.map((n) => n.id));
      draw();
    } else if (ctrl && e.key === 'c') copy();
    else if (ctrl && e.key === 'x') {
      copy();
      apply('Cut', (ns) => M.deleteNotes(ns, S.sel));
    } else if (ctrl && e.key === 'v') pasteAt(S.view.x0 + 20 / S.view.ppt);
    else if (ctrl && e.key === 'd') dupSel();
    else if (e.key === 'ArrowUp' && S.sel.size) apply('Transpose', (ns) => M.transpose(ns, S.sel, e.shiftKey ? 12 : 1));
    else if (e.key === 'ArrowDown' && S.sel.size) apply('Transpose', (ns) => M.transpose(ns, S.sel, e.shiftKey ? -12 : -1));
    else if (e.key === 'ArrowRight' && S.sel.size) apply('Move notes', (ns) => M.moveNotes(ns, S.sel, snap, 0));
    else if (e.key === 'ArrowLeft' && S.sel.size) apply('Move notes', (ns) => M.moveNotes(ns, S.sel, -snap, 0));
    else if (e.key === 'q' && !ctrl) apply('Quantize', (ns) => M.quantize(ns, S.sel, snap));
    else if (e.key === 'p' && !ctrl) setTool('draw');
    else if (e.key === 'e' && !ctrl) setTool('select');
    else if (e.key === 'd' && !ctrl) setTool('erase');
    else return;
    e.preventDefault();
    e.stopPropagation();
  };

  if (!project || !pat) return null;
  const zoom = (f: number) => {
    const xc = KW + (S.size.w - KW) / 2, t = tickAt(xc);
    S.view.ppt = clamp(S.view.ppt * f, 0.01, 1.5);
    S.view.x0 = Math.max(0, t - (xc - KW) / S.view.ppt);
    draw();
  };
  return (
    <div class="panel" style={{ flex: 1 }}>
      <div class="panel-head">
        <span class="panel-title">Piano roll</span>
        <select
          class="mini"
          value={chan?.id}
          aria-label="Channel"
          onChange={(e) => ui.set({ pianoChannel: +(e.target as HTMLSelectElement).value, selectedChannel: +(e.target as HTMLSelectElement).value })}
        >
          {project.channels.map((c) => (
            <option key={c.id} value={c.id}>
              {c.name}
            </option>
          ))}
        </select>
        <span class="hint">{pat.name}</span>
        <div class="sep" />
        <Btn class="icon" active={tool === 'draw'} title="Draw (P)" onClick={() => setTool('draw')}>
          <Icon name="pencil" />
        </Btn>
        <Btn class="icon" active={tool === 'select'} title="Select (E)" onClick={() => setTool('select')}>
          <Icon name="cursor" />
        </Btn>
        <Btn class="icon" active={tool === 'erase'} title="Erase (D)" onClick={() => setTool('erase')}>
          <Icon name="eraser" />
        </Btn>
        <Btn
          class="icon"
          active={tool === 'slide'}
          disabled={!info?.capabilities.slide}
          title={info?.capabilities.slide ? 'Slide notes: glide into the note from the one before' : `${info?.name ?? 'This instrument'} does not support slides`}
          onClick={() => setTool('slide')}
        >
          <Icon name="slide" />
        </Btn>
        <div class="sep" />
        <Icon name="magnet" />
        <select class="mini" value={snapI} aria-label="Snap" onChange={(e) => setSnapI(+(e.target as HTMLSelectElement).value)}>
          {M.SNAPS.map((s, i) => (
            <option key={i} value={i}>
              {s.label}
            </option>
          ))}
        </select>
        <Btn class="icon flat" title="Zoom out" onClick={() => zoom(1 / 1.4)}>
          <Icon name="zoomout" />
        </Btn>
        <Btn class="icon flat" title="Zoom in" onClick={() => zoom(1.4)}>
          <Icon name="zoomin" />
        </Btn>
        <Btn class="icon" active={follow} title="Follow the playhead" onClick={() => ui.set({ follow: !follow })}>
          <Icon name="follow" />
        </Btn>
        <div class="sep" />
        <div class="lane-sel">
          {LANES.map((l) => (
            <Btn key={l.id} class="icon" active={lane === l.id} title={laneAvailable(l.id) ? l.label : `${l.label}: not supported by ${info?.name ?? 'this instrument'}`} onClick={() => setLane(l.id)}>
              <span style={laneAvailable(l.id) ? {} : { opacity: 0.45 }}>{l.label.split(' ')[0].slice(0, 4)}</span>
            </Btn>
          ))}
        </div>
        {!p.mobile ? (
          <Btn class="icon" title="Import a MIDI file into this channel" onClick={() => importMidi('piano')}>
            <Icon name="import" />
          </Btn>
        ) : null}
        <Btn class="icon" title="Note tools" onClick={(e) => menuAt(e, opsMenu())}>
          <Icon name="more" />
        </Btn>
        <div class="spacer" />
        {!p.mobile ? (
          <Btn class="icon flat" title="Close" onClick={() => togglePanel('piano')}>
            <Icon name="x" />
          </Btn>
        ) : null}
      </div>
      <div class="panel-body">
        <div
          class="cv-wrap"
          ref={wrap}
          tabIndex={0}
          onKeyDown={onKey}
          style={{ outline: 'none' }}
          onDragOver={(e) => e.dataTransfer?.types.includes('Files') && e.preventDefault()}
          onDrop={(e) => {
            const f = [...(e.dataTransfer?.files ?? [])].find(isMidiFile);
            if (!f) return;
            e.preventDefault();
            importMidi('piano', f);
          }}
        >
          <canvas
            ref={cv}
            onPointerDown={onDown}
            onPointerMove={onMove}
            onPointerUp={onUp}
            onPointerCancel={onUp}
            onWheel={onWheel}
            onContextMenu={(e) => e.preventDefault()}
            aria-label="Piano roll grid"
          />
          <canvas ref={ov} style={{ pointerEvents: 'none' }} />
          {chan && !S.notes.length && !(pat.seqs.find((s) => s.channel === chan.id)?.pianoRoll) && (pat.seqs.find((s) => s.channel === chan.id)?.steps.some((x) => x[0])) ? (
            <div class="roll-hint">
              <span>
                <b>{chan.name}</b> plays steps in “{pat.name}”.
              </span>
              <Btn active onClick={() => A.setSeqMode(pat.id, chan.id, true)}>
                Convert the steps to notes
              </Btn>
              <span class="hint">or draw notes: the row then plays the piano roll</span>
            </div>
          ) : null}
        </div>
      </div>
      {props ? <NoteProps note={props} info={info} onClose={() => setProps(null)} onApply={(n) => apply('Note properties', (ns) => ns.map((m) => (m.id === n.id ? n : m)))} /> : null}
    </div>
  );
}

function NoteProps(p: { note: Note; info?: PluginInfo; onClose: () => void; onApply: (n: Note) => void }) {
  const [n, setN] = useState(p.note);
  const caps = p.info?.capabilities ?? {};
  const row = (label: string, k: keyof Note, min: number, max: number, ok = true, fmt = (v: number) => v.toFixed(2)) => (
    <div class="pe-ctl" title={ok ? '' : `${p.info?.name ?? 'This instrument'} ignores this value`} style={ok ? {} : { opacity: 0.45 }}>
      <Knob size={40} value={n[k] as number} min={min} max={max} scale={min < 0 ? 'bipolar' : 'linear'} onChange={(v) => setN({ ...n, [k]: v })} />
      <span class="knob-label">{label}</span>
      <span class="pe-val">{fmt(n[k] as number)}</span>
    </div>
  );
  return (
    <Modal title={`Note ${noteName(n.key)} — properties`} onClose={p.onClose}>
      <div class="pe-ctls">
        {row('Velocity', 'vel', 0, 1, true, (v) => `${Math.round(v * 100)} %`)}
        {row('Pan', 'pan', -1, 1, !!caps.perNotePan, (v) => (v === 0 ? 'C' : v < 0 ? `L${Math.round(-v * 100)}` : `R${Math.round(v * 100)}`))}
        {row('Fine pitch', 'pitch', -1, 1, !!caps.perNotePitch, (v) => `${Math.round(v * 100)} ct`)}
        {row('Release', 'rel', 0, 1, !!caps.releaseVelocity)}
        {row('Probability', 'prob', 0, 1, true, (v) => `${Math.round(v * 100)} %`)}
      </div>
      <div class="row" style={{ marginTop: '10px' }}>
        <label>
          <input type="checkbox" checked={n.slide} disabled={!caps.slide} onChange={() => setN({ ...n, slide: !n.slide })} /> Slide{caps.slide ? '' : ' (not supported)'}
        </label>
        <label>
          <input type="checkbox" checked={n.accent} onChange={() => setN({ ...n, accent: !n.accent })} /> Accent
        </label>
        <div class="spacer" />
        <Btn onClick={p.onClose}>Cancel</Btn>
        <Btn
          active
          onClick={() => {
            p.onApply(n);
            p.onClose();
          }}
        >
          Apply
        </Btn>
      </div>
    </Modal>
  );
}

function roundRect(g: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number) {
  g.beginPath();
  g.moveTo(x + r, y);
  g.lineTo(x + w - r, y);
  g.quadraticCurveTo(x + w, y, x + w, y + r);
  g.lineTo(x + w, y + h - r);
  g.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
  g.lineTo(x + r, y + h);
  g.quadraticCurveTo(x, y + h, x, y + h - r);
  g.lineTo(x, y + r);
  g.quadraticCurveTo(x, y, x + r, y);
  g.closePath();
}

void P;
