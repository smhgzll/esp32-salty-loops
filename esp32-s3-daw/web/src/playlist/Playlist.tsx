// SPDX-License-Identifier: GPL-3.0-or-later
// Playlist / song arrangement: pattern and automation clips on tracks. A pattern clip references its
// pattern (no copy of the notes): editing the pattern changes every clip of it. A clip longer than
// its pattern repeats it; clips are cut at their end.
//   draw: click = place the chosen pattern / automation (drag to move it); double-click an automation clip = its point editor;  drag a clip = move,
//   its right edge = length, Ctrl-drag = copy;  right-click = delete;  slice tool: split;
//   ruler: click = jump, drag = loop range;  touch: two fingers pan / zoom, long press = clip menu
import { useEffect, useRef, useState } from 'preact/hooks';

import { useCanvasLive } from '../app/canvas';

import * as A from '../app/actions';
import { send } from '../app/protocol';
import { barTicks, live, proj, ui } from '../app/state';
import { useStore } from '../app/store';
import type { Clip, Project } from '../app/types';
import { clamp, rgba, shade } from '../app/util';
import { Btn, Icon, Led, menuAt, showMenu } from '../ui/controls';
import { togglePanel } from '../transport/TopBar';
import { openAutomation } from './AutomationEditor';
import { importMidi, isMidiFile } from '../midi/MidiImport';

type Tool = 'draw' | 'select' | 'slice' | 'erase';
const SNAPS = [
  { label: 'Bar', f: 1 },
  { label: 'Beat', f: 0.25 },
  { label: 'Step', f: 1 / 16 },
  { label: 'None', f: 0 },
];

interface View {
  x0: number;
  ppt: number;
  th: number; // track height
  y0: number; // first visible track (fractional)
}

export function Playlist(p: { mobile?: boolean }) {
  const project = useStore(proj, (s) => s.project);
  const follow = useStore(ui, (s) => s.follow);
  const wrap = useRef<HTMLDivElement>(null);
  const cv = useRef<HTMLCanvasElement>(null);
  const ov = useRef<HTMLCanvasElement>(null);
  const [tool, setTool] = useState<Tool>('draw');
  const [snapI, setSnapI] = useState(0);
  const [brush, setBrush] = useState<{ kind: 'pattern' | 'automation'; ref: number } | null>(null);
  const HW = p.mobile ? 74 : 132, RH = 24;
  const st = useRef({
    view: { x0: 0, ppt: p.mobile ? 0.012 : 0.018, th: p.mobile ? 44 : 38, y0: 0 } as View,
    clips: [] as Clip[],
    sel: new Set<number>(),
    g: null as null | { kind: string; [k: string]: any },
    pointers: new Map<number, { x: number; y: number }>(),
    pinch: null as null | { d0: number; cx: number; v: View; cy: number },
    lastTap: { id: -1, t: 0 },
    lp: null as ReturnType<typeof setTimeout> | null,
    size: { w: 0, h: 0 },
  });
  const S = st.current;
  if (!S.g && project) S.clips = project.playlist.clips;

  const bar = () => barTicks();
  const snapT = () => (SNAPS[snapI].f ? Math.round(bar() * SNAPS[snapI].f) : 1);
  const snapDown = (t: number) => (snapT() <= 1 ? Math.round(t) : Math.floor(t / snapT()) * snapT());
  const snapNear = (t: number) => (snapT() <= 1 ? Math.round(t) : Math.round(t / snapT()) * snapT());
  const tickAt = (x: number) => S.view.x0 + (x - HW) / S.view.ppt;
  const xOf = (t: number) => HW + (t - S.view.x0) * S.view.ppt;
  const trackAt = (y: number) => Math.floor(S.view.y0 + (y - RH) / S.view.th);
  const yOf = (tr: number) => RH + (tr - S.view.y0) * S.view.th;
  const brushOf = () => brush ?? (project ? { kind: 'pattern' as const, ref: project.meta.currentPattern } : null);
  const clipLabel = (pr: Project, c: Clip) =>
    c.kind === 'pattern' ? pr.patterns.find((x) => x.id === c.ref)?.name ?? '?' : pr.automation.find((x) => x.id === c.ref)?.name ?? 'Automation';
  const clipColor = (pr: Project, c: Clip) =>
    c.kind === 'pattern' ? pr.patterns.find((x) => x.id === c.ref)?.color ?? '#888' : pr.automation.find((x) => x.id === c.ref)?.color ?? '#40c4aa';
  const clipAt = (x: number, y: number): [Clip | null, 'body' | 'edge'] => {
    const t = tickAt(x), tr = trackAt(y);
    for (let i = S.clips.length - 1; i >= 0; i--) {
      const c = S.clips[i];
      if (c.track === tr && t >= c.start && t < c.start + c.len) return [c, xOf(c.start + c.len) - x < Math.min(10, c.len * S.view.ppt * 0.3) ? 'edge' : 'body'];
    }
    return [null, 'body'];
  };
  const defaultLen = (b: { kind: string; ref: number }) => {
    if (!project) return bar();
    if (b.kind === 'pattern') return project.patterns.find((x) => x.id === b.ref)?.effLength || bar();
    return project.automation.find((x) => x.id === b.ref)?.length || bar() * 4;
  };

  // ---- drawing ------------------------------------------------------------------------------
  const draw = () => {
    const c = cv.current;
    if (!c || !project) return;
    const dpr = window.devicePixelRatio || 1;
    const { w, h } = S.size;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      for (const el of [c, ov.current!]) {
        el.width = Math.round(w * dpr);
        el.height = Math.round(h * dpr);
        el.style.width = `${w}px`;
        el.style.height = `${h}px`;
      }
    }
    const g = c.getContext('2d')!;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    const v = S.view, b = bar(), beat = b / project.meta.timeSig[0];
    const tracks = project.playlist.tracks;
    g.fillStyle = '#1b1f23';
    g.fillRect(0, 0, w, h);
    // track lanes
    for (let tr = Math.floor(v.y0); tr < tracks.length && yOf(tr) < h; tr++) {
      const y = yOf(tr);
      g.fillStyle = tr % 2 ? '#22272b' : '#262b30';
      g.fillRect(HW, y, w - HW, v.th);
      g.fillStyle = '#16191c';
      g.fillRect(HW, y + v.th - 1, w - HW, 1);
    }
    // grid
    const t0 = Math.max(0, v.x0), t1 = tickAt(w);
    const fine = beat * v.ppt >= 10 ? beat : b;
    for (let t = Math.floor(t0 / fine) * fine; t <= t1; t += fine) {
      g.fillStyle = t % b === 0 ? '#121417' : '#1d2125';
      g.fillRect(Math.round(xOf(t)), RH, 1, h - RH);
    }
    // loop range
    const lp = project.meta.loop;
    if (lp.end > lp.start) {
      g.fillStyle = lp.on ? 'rgba(255,154,60,0.07)' : 'rgba(255,255,255,0.03)';
      g.fillRect(xOf(lp.start), RH, (lp.end - lp.start) * v.ppt, h - RH);
    }
    // clips
    g.save();
    g.beginPath();
    g.rect(HW, RH, w - HW, h - RH);
    g.clip();
    g.font = '11px system-ui';
    g.textBaseline = 'top';
    for (const cl of S.clips) {
      const x = xOf(cl.start), y = yOf(cl.track), wd = Math.max(4, cl.len * v.ppt);
      if (x > w || x + wd < HW || y > h || y + v.th < RH) continue;
      const col = clipColor(project, cl);
      const sel = S.sel.has(cl.id);
      const muted = cl.muted || tracks[cl.track]?.mute;
      g.fillStyle = muted ? '#3a3f44' : rgba(col, sel ? 0.55 : 0.32);
      g.fillRect(x, y + 1, wd - 1, v.th - 2);
      g.fillStyle = muted ? '#555' : sel ? shade(col, 0.3) : col;
      g.fillRect(x, y + 1, wd - 1, 13);
      g.strokeStyle = sel ? '#fff' : 'rgba(0,0,0,0.8)';
      g.strokeRect(x + 0.5, y + 1.5, wd - 2, v.th - 3);
      g.fillStyle = '#111';
      g.fillText(clipLabel(project, cl), x + 4, y + 2, Math.max(0, wd - 8));
      // contents preview
      g.save();
      g.beginPath();
      g.rect(x, y + 14, wd - 1, v.th - 16);
      g.clip();
      if (cl.kind === 'pattern') drawPatternPreview(g, project, cl, x, y + 15, v.th - 18, v.ppt, muted ? '#777' : shade(col, 0.5));
      else drawAutoPreview(g, project, cl, x, y + 15, v.th - 18, v.ppt, shade(col, 0.5));
      g.restore();
      // repetitions of the pattern inside the clip
      if (cl.kind === 'pattern') {
        const L = project.patterns.find((x2) => x2.id === cl.ref)?.effLength || b;
        g.fillStyle = 'rgba(0,0,0,0.5)';
        for (let t = L - (cl.offset % L); t < cl.len; t += L) g.fillRect(x + t * v.ppt, y + 14, 1, v.th - 15);
      }
    }
    if (S.g?.kind === 'rect') {
      const r = S.g;
      g.strokeStyle = '#ff9a3c';
      g.fillStyle = 'rgba(255,154,60,0.12)';
      const xa = Math.min(r.x0, r.x1), ya = Math.min(r.y0, r.y1);
      g.fillRect(xa, ya, Math.abs(r.x1 - r.x0), Math.abs(r.y1 - r.y0));
      g.strokeRect(xa + 0.5, ya + 0.5, Math.abs(r.x1 - r.x0), Math.abs(r.y1 - r.y0));
    }
    g.restore();
    // headers
    for (let tr = Math.floor(v.y0); tr < tracks.length && yOf(tr) < h; tr++) {
      const y = yOf(tr), t = tracks[tr];
      g.fillStyle = '#2b3036';
      g.fillRect(0, y, HW, v.th);
      g.fillStyle = t.color;
      g.fillRect(0, y, 4, v.th - 1);
      g.fillStyle = '#121417';
      g.fillRect(0, y + v.th - 1, HW, 1);
      g.fillStyle = t.mute ? '#6b757f' : '#d6dbe0';
      g.font = '11px system-ui';
      g.textBaseline = 'middle';
      g.fillText(t.name, 26, y + v.th / 2, HW - 46);
      // mute led
      g.fillStyle = t.mute ? '#3a2b20' : '#5fd068';
      g.beginPath();
      g.arc(14, y + v.th / 2, 5, 0, Math.PI * 2);
      g.fill();
      if (t.solo) {
        g.fillStyle = '#ff9a3c';
        g.fillText('S', HW - 14, y + v.th / 2);
      }
    }
    g.fillStyle = '#0b0d0f';
    g.fillRect(HW - 1, RH, 1, h);
    // ruler
    g.fillStyle = '#2a2f35';
    g.fillRect(0, 0, w, RH);
    g.fillStyle = '#0b0d0f';
    g.fillRect(0, RH - 1, w, 1);
    if (lp.end > lp.start) {
      g.fillStyle = lp.on ? '#ff9a3c' : '#6b757f';
      g.fillRect(xOf(lp.start), 0, (lp.end - lp.start) * v.ppt, 5);
    }
    g.fillStyle = '#c0c6cc';
    g.font = '10px system-ui';
    g.textBaseline = 'alphabetic';
    const every = b * v.ppt < 26 ? 8 : b * v.ppt < 50 ? 4 : 1;
    for (let t = Math.floor(t0 / b) * b; t <= t1; t += b) {
      const x = xOf(t), n = t / b;
      if (x < HW) continue;
      g.fillRect(x, n % every ? 16 : 8, 1, RH);
      if (n % every === 0) g.fillText(String(n + 1), x + 3, 16);
    }
    for (const m of project.playlist.markers) {
      const x = xOf(m.tick);
      if (x < HW) continue;
      g.fillStyle = '#5fb3ff';
      g.fillRect(x, 0, 1, RH);
      g.fillText(m.name, x + 3, 22);
    }
    g.fillStyle = '#9aa4ad';
    g.fillText('Tracks', 8, 16);
    drawPlayhead();
  };

  const drawPlayhead = () => {
    const o = ov.current;
    if (!o || S.size.w <= HW || S.size.h <= RH) return;
    const g = o.getContext('2d')!;
    const dpr = window.devicePixelRatio || 1;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, S.size.w, S.size.h);
    const s = live.state.status;
    if (!s || s.mode !== 'song') return;
    const x = xOf(s.tick);
    if (follow && s.play === 1 && (x > S.size.w - 30 || x < HW)) {
      const x0 = Math.max(0, s.tick - 40 / S.view.ppt);
      // Layout can temporarily leave too little room for the follow margin.
      // Redrawing an unchanged view would recurse through drawPlayhead forever.
      if (x0 !== S.view.x0) {
        S.view.x0 = x0;
        draw();
        return;
      }
    }
    if (x < HW || x > S.size.w) return;
    g.fillStyle = s.play ? '#7de08a' : '#5fb3ff';
    g.fillRect(x, 0, 1.5, S.size.h);
    g.beginPath();
    g.moveTo(x - 5, 0);
    g.lineTo(x + 6, 0);
    g.lineTo(x + 0.5, 7);
    g.fill();
  };

  useCanvasLive(wrap, (el) => {
      S.size = { w: el.clientWidth, h: el.clientHeight };
      draw();
  }, drawPlayhead);
  useEffect(draw);

  // ---- editing --------------------------------------------------------------------------------
  const commit = (label: string, before: Clip[]) => A.setClips(S.clips, label, before);
  const nextId = () => S.clips.reduce((m, c) => Math.max(m, c.id), 0) + 1 + Math.floor(Math.random() * 1000);
  const clipMenu = (cl: Clip, x: number, y: number) =>
    showMenu(
      [
        cl.kind === 'automation'
          ? { label: 'Edit automation…', action: () => openAutomation(cl.ref) }
          : { label: 'Open pattern', action: () => A.selectPattern(cl.ref, false) },
        { label: cl.muted ? 'Unmute clip' : 'Mute clip', action: () => { const before = S.clips; S.clips = S.clips.map((c) => (c.id === cl.id ? { ...c, muted: !c.muted } : c)); commit('Mute clip', before); } },
        { label: 'Duplicate after', action: () => { const before = S.clips; S.clips = [...S.clips, { ...cl, id: nextId(), start: cl.start + cl.len }]; commit('Duplicate clip', before); } },
        { label: 'Split at the playhead', action: () => split(cl, live.state.status?.tick ?? 0) },
        { label: 'Make unique (clone the pattern)', disabled: cl.kind !== 'pattern', action: () => makeUnique(cl) },
        { label: 'Delete', danger: true, action: () => { const before = S.clips; S.clips = S.clips.filter((c) => c.id !== cl.id); commit('Delete clip', before); } },
      ],
      x,
      y,
    );
  const split = (cl: Clip, at: number) => {
    at = snapNear(at);
    if (at <= cl.start || at >= cl.start + cl.len) return;
    const before = S.clips;
    S.clips = S.clips.flatMap((c) =>
      c.id === cl.id ? [{ ...c, len: at - c.start }, { ...c, id: nextId(), start: at, len: c.start + c.len - at, offset: c.offset + (at - c.start) }] : [c],
    );
    commit('Split clip', before);
  };
  const makeUnique = async (cl: Clip) => {
    const pr = proj.state.project;
    if (!pr) return;
    const n = pr.patterns.length;
    A.dupPattern(cl.ref);
    // the new pattern appears after the original; point the clip at it once it exists
    const t = setInterval(() => {
      const p2 = proj.state.project;
      if (p2 && p2.patterns.length > n) {
        clearInterval(t);
        const i = p2.patterns.findIndex((x) => x.id === cl.ref);
        const np = p2.patterns[i + 1];
        if (np) {
          const before = p2.playlist.clips;
          A.setClips(before.map((c) => (c.id === cl.id ? { ...c, ref: np.id } : c)), 'Make unique', before);
        }
      }
    }, 50);
    setTimeout(() => clearInterval(t), 3000);
  };

  // ---- pointer input ----------------------------------------------------------------------------
  const local = (e: PointerEvent) => {
    const r = cv.current!.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };
  const onDown = (e: PointerEvent) => {
    if (!project) return;
    const { x, y } = local(e);
    S.pointers.set(e.pointerId, { x, y });
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    if (S.pointers.size === 2) {
      if (S.lp) clearTimeout(S.lp);
      if (S.g?.before) S.clips = S.g.before;
      S.g = null;
      const [a, b] = [...S.pointers.values()];
      S.pinch = { d0: Math.abs(a.x - b.x), cx: (a.x + b.x) / 2, cy: (a.y + b.y) / 2, v: { ...S.view } };
      draw();
      return;
    }
    const t = tickAt(x);
    if (y < RH) {
      S.g = { kind: 'ruler', t0: Math.max(0, snapDown(t)), moved: false };
      return;
    }
    const tr = trackAt(y);
    if (x < HW) {
      const t2 = project.playlist.tracks[tr];
      if (!t2) return;
      if (x < 24) A.setPlaylistTrack(tr, { mute: !t2.mute });
      else
        showMenu(
          [
            { label: 'Rename…', action: () => { const n = prompt('Track name', t2.name); if (n) A.setPlaylistTrack(tr, { name: n }); } },
            { label: t2.mute ? 'Unmute' : 'Mute', action: () => A.setPlaylistTrack(tr, { mute: !t2.mute }) },
            { label: t2.solo ? 'Unsolo' : 'Solo', action: () => A.setPlaylistTrack(tr, { solo: !t2.solo }) },
            { label: 'Add track', action: A.addPlaylistTrack },
          ],
          e.clientX,
          e.clientY,
        );
      return;
    }
    if (e.button === 1) {
      S.g = { kind: 'pan', x, y, v: { ...S.view } };
      return;
    }
    const [hit, part] = clipAt(x, y);
    if (e.button === 2 || tool === 'erase') {
      S.g = { kind: 'erase', before: S.clips };
      if (hit) S.clips = S.clips.filter((c) => c.id !== hit.id);
      draw();
      return;
    }
    if (tool === 'slice') {
      if (hit) split(hit, t);
      return;
    }
    if (hit) {
      // double click / double tap on an automation clip: its point editor
      const now = Date.now();
      if (hit.kind === 'automation' && S.lastTap.id === hit.id && now - S.lastTap.t < 320) {
        S.lastTap = { id: -1, t: 0 };
        openAutomation(hit.ref);
        return;
      }
      S.lastTap = { id: hit.id, t: now };
      if (!S.sel.has(hit.id)) S.sel = e.shiftKey ? new Set([...S.sel, hit.id]) : new Set([hit.id]);
      S.g = { kind: part === 'edge' ? 'resize' : e.ctrlKey || e.metaKey ? 'dup' : 'move', id: hit.id, t0: t, tr0: tr, before: S.clips, moved: false, px: x, py: y };
      if (e.pointerType === 'touch')
        S.lp = setTimeout(() => {
          S.lp = null;
          if (S.g && !S.g.moved) {
            S.g = null;
            clipMenu(hit, e.clientX, e.clientY);
          }
        }, 520);
      draw();
      return;
    }
    if (tool === 'select' || e.shiftKey) {
      S.g = { kind: 'rect', x0: x, y0: y, x1: x, y1: y };
      S.sel = new Set();
      draw();
      return;
    }
    const b = brushOf();
    if (!b || tr < 0 || tr >= project.playlist.tracks.length) return;
    const before = S.clips;
    const nc: Clip = { id: nextId(), kind: b.kind, track: tr, start: Math.max(0, snapDown(t)), len: defaultLen(b), offset: 0, ref: b.ref };
    S.clips = [...S.clips, nc];
    S.sel = new Set([nc.id]);
    S.g = { kind: 'move', id: nc.id, t0: t, tr0: tr, before, moved: true, created: true, base: S.clips, px: x, py: y };
    draw();
  };
  const onMove = (e: PointerEvent) => {
    const pt = local(e);
    if (S.pointers.has(e.pointerId)) S.pointers.set(e.pointerId, pt);
    if (S.pinch && S.pointers.size >= 2) {
      const [a, b] = [...S.pointers.values()];
      const pz = S.pinch;
      const d = Math.abs(a.x - b.x), cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
      const v = { ...pz.v };
      if (pz.d0 > 40) v.ppt = clamp(pz.v.ppt * (d / pz.d0), 0.002, 0.3);
      const tC = pz.v.x0 + (pz.cx - HW) / pz.v.ppt;
      v.x0 = Math.max(0, tC - (cx - HW) / v.ppt);
      v.y0 = clamp(pz.v.y0 - (cy - pz.cy) / v.th, 0, Math.max(0, (proj.state.project?.playlist.tracks.length ?? 1) - 1));
      S.view = v;
      draw();
      return;
    }
    const g = S.g;
    if (!g) return;
    const { x, y } = pt;
    const t = tickAt(x);
    if (g.kind === 'pan') {
      S.view.x0 = Math.max(0, g.v.x0 - (x - g.x) / S.view.ppt);
      S.view.y0 = Math.max(0, g.v.y0 - (y - g.y) / S.view.th);
    } else if (g.kind === 'ruler') {
      g.moved = g.moved || Math.abs(t - g.t0) * S.view.ppt > 4;
      g.t1 = Math.max(0, snapNear(t));
    } else if (g.kind === 'erase') {
      const [hit] = clipAt(x, y);
      if (hit) S.clips = S.clips.filter((c) => c.id !== hit.id);
    } else if (g.kind === 'rect') {
      g.x1 = x;
      g.y1 = y;
      const ta = tickAt(Math.min(g.x0, x)), tb = tickAt(Math.max(g.x0, x)), ra = trackAt(Math.min(g.y0, y)), rb = trackAt(Math.max(g.y0, y));
      S.sel = new Set(S.clips.filter((c) => c.start < tb && c.start + c.len > ta && c.track >= ra && c.track <= rb).map((c) => c.id));
    } else {
      if (!g.moved && Math.abs(x - g.px) < 3 && Math.abs(y - g.py) < 3) return;
      g.moved = true;
      if (S.lp) clearTimeout(S.lp);
      const base: Clip[] = g.base ?? g.before;
      const anchor = base.find((c) => c.id === g.id)!;
      const ids = g.created ? new Set([g.id]) : S.sel;
      if (g.kind === 'resize') {
        const dl = snapNear(t - g.t0);
        S.clips = base.map((c) => (ids.has(c.id) ? { ...c, len: Math.max(snapT() > 1 ? snapT() : 60, c.len + dl) } : c));
      } else {
        const dt = snapNear(anchor.start + (t - g.t0)) - anchor.start;
        const ntr = proj.state.project?.playlist.tracks.length ?? 1;
        let dtr = trackAt(y) - g.tr0;
        for (const c of base) if (ids.has(c.id)) dtr = clamp(dtr, -c.track, ntr - 1 - c.track);
        let minStart = Infinity;
        for (const c of base) if (ids.has(c.id)) minStart = Math.min(minStart, c.start);
        const ddt = Math.max(dt, -minStart);
        if (g.kind === 'dup') {
          let id = nextId();
          const copies = base.filter((c) => ids.has(c.id)).map((c) => ({ ...c, id: id++, start: c.start + ddt, track: c.track + dtr }));
          S.clips = [...base, ...copies];
          S.sel = new Set(copies.map((c) => c.id));
        } else S.clips = base.map((c) => (ids.has(c.id) ? { ...c, start: c.start + ddt, track: c.track + dtr } : c));
      }
    }
    draw();
  };
  const onUp = (e: PointerEvent) => {
    S.pointers.delete(e.pointerId);
    if (S.lp) clearTimeout(S.lp);
    if (S.pinch) {
      if (S.pointers.size < 2) S.pinch = null;
      return;
    }
    const g = S.g;
    S.g = null;
    if (!g) return;
    if (g.kind === 'ruler') {
      if (g.moved && g.t1 !== undefined && g.t1 !== g.t0) A.setLoop(true, Math.min(g.t0, g.t1), Math.max(g.t0, g.t1));
      else A.seek(g.t0);
    } else if (g.kind === 'erase' && S.clips.length !== g.before.length) commit('Delete clips', g.before);
    else if ((g.kind === 'move' || g.kind === 'dup' || g.kind === 'resize') && g.moved)
      commit(g.created ? 'Add clip' : g.kind === 'resize' ? 'Resize clips' : g.kind === 'dup' ? 'Copy clips' : 'Move clips', g.before);
    draw();
  };
  const onWheel = (e: WheelEvent) => {
    e.preventDefault();
    const x = e.clientX - cv.current!.getBoundingClientRect().left;
    if (e.ctrlKey || e.metaKey) {
      const t = tickAt(x);
      S.view.ppt = clamp(S.view.ppt * (e.deltaY < 0 ? 1.15 : 1 / 1.15), 0.002, 0.3);
      S.view.x0 = Math.max(0, t - (x - HW) / S.view.ppt);
    } else if (e.altKey) S.view.th = clamp(S.view.th * (e.deltaY < 0 ? 1.1 : 1 / 1.1), 22, 120);
    else if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) S.view.x0 = Math.max(0, S.view.x0 + (e.deltaX || e.deltaY) / S.view.ppt);
    else S.view.y0 = clamp(S.view.y0 + e.deltaY / S.view.th / 2, 0, Math.max(0, (project?.playlist.tracks.length ?? 1) - 1));
    draw();
  };
  const onKey = (e: KeyboardEvent) => {
    if ((e.key === 'Delete' || e.key === 'Backspace') && S.sel.size) {
      const before = S.clips;
      S.clips = S.clips.filter((c) => !S.sel.has(c.id));
      S.sel = new Set();
      commit('Delete clips', before);
      e.preventDefault();
    } else if ((e.ctrlKey || e.metaKey) && e.key === 'a') {
      S.sel = new Set(S.clips.map((c) => c.id));
      draw();
      e.preventDefault();
    }
  };

  if (!project) return null;
  const b = brushOf();
  return (
    <div class="panel" style={{ flex: 1 }}>
      <div class="panel-head">
        <span class="panel-title">Playlist</span>
        <Btn class="icon" active={tool === 'draw'} title="Draw clips" onClick={() => setTool('draw')}>
          <Icon name="pencil" />
        </Btn>
        <Btn class="icon" active={tool === 'select'} title="Select" onClick={() => setTool('select')}>
          <Icon name="cursor" />
        </Btn>
        <Btn class="icon" active={tool === 'slice'} title="Slice clips" onClick={() => setTool('slice')}>
          <Icon name="scissors" />
        </Btn>
        <Btn class="icon" active={tool === 'erase'} title="Delete clips" onClick={() => setTool('erase')}>
          <Icon name="eraser" />
        </Btn>
        <div class="sep" />
        <Icon name="magnet" />
        <select class="mini" value={snapI} aria-label="Snap" onChange={(e) => setSnapI(+(e.target as HTMLSelectElement).value)}>
          {SNAPS.map((s, i) => (
            <option key={i} value={i}>
              {s.label}
            </option>
          ))}
        </select>
        <div class="sep" />
        <span class="hint">Draw:</span>
        <select
          class="mini"
          aria-label="Clip to draw"
          value={b ? `${b.kind}:${b.ref}` : ''}
          onChange={(e) => {
            const [k, r] = (e.target as HTMLSelectElement).value.split(':');
            setBrush({ kind: k as 'pattern' | 'automation', ref: +r });
          }}
        >
          {project.patterns.map((x) => (
            <option key={x.id} value={`pattern:${x.id}`}>
              {x.name}
            </option>
          ))}
          {project.automation.map((x) => (
            <option key={x.id} value={`automation:${x.id}`}>
              ⤳ {x.name}
            </option>
          ))}
        </select>
        <Btn class="icon flat" title="Zoom out" onClick={() => { S.view.ppt = clamp(S.view.ppt / 1.4, 0.002, 0.3); draw(); }}>
          <Icon name="zoomout" />
        </Btn>
        <Btn class="icon flat" title="Zoom in" onClick={() => { S.view.ppt = clamp(S.view.ppt * 1.4, 0.002, 0.3); draw(); }}>
          <Icon name="zoomin" />
        </Btn>
        <Btn class="icon" active={follow} title="Follow the playhead" onClick={() => ui.set({ follow: !follow })}>
          <Icon name="follow" />
        </Btn>
        <Btn class="icon" title="More" onClick={(e) => menuAt(e, [
          { label: 'Add track', action: A.addPlaylistTrack },
          { label: 'Add marker at the playhead', action: () => send('marker', { action: 'add', tick: live.state.status?.tick ?? 0, name: `M${project.playlist.markers.length + 1}` }) },
          { label: 'Loop the whole song', action: () => A.setLoop(true, 0, Math.max(barTicks(), ...S.clips.map((c) => c.start + c.len))) },
          { label: 'Loop off', action: () => A.setLoop(false) },
        ])}>
          <Icon name="more" />
        </Btn>
        <div class="spacer" />
        <Led on={false} onClick={() => A.setMode('song')} title="Switch the transport to song mode" />
        <span class="hint">song</span>
        {!p.mobile ? (
          <Btn class="icon flat" title="Close" onClick={() => togglePanel('playlist')}>
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
            // a MIDI file dropped on the playlist: import it as a project
            const f = [...(e.dataTransfer?.files ?? [])].find(isMidiFile);
            if (!f) return;
            e.preventDefault();
            importMidi('project', f);
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
            aria-label="Playlist"
          />
          <canvas ref={ov} style={{ pointerEvents: 'none' }} />
        </div>
      </div>
    </div>
  );
}

function drawPatternPreview(g: CanvasRenderingContext2D, pr: Project, cl: Clip, x: number, y: number, h: number, ppt: number, color: string) {
  const pat = pr.patterns.find((p) => p.id === cl.ref);
  if (!pat || h < 4) return;
  const L = pat.effLength || 3840;
  let lo = 127, hi = 0;
  const notes: [number, number, number][] = [];
  for (const s of pat.seqs) {
    if (s.pianoRoll) for (const n of s.notes) notes.push([n[1], n[2], n[3]]);
    else s.steps.forEach((st, i) => st[0] && notes.push([i * 240, 120, 30 + (pr.channels.findIndex((c) => c.id === s.channel) % 12) * 4]));
  }
  for (const n of notes) {
    lo = Math.min(lo, n[2]);
    hi = Math.max(hi, n[2]);
  }
  const range = Math.max(8, hi - lo + 1);
  g.fillStyle = color;
  for (let rep = Math.floor(cl.offset / L) * L - cl.offset; rep < cl.len; rep += L)
    for (const n of notes) {
      const t = rep + n[0];
      if (t + n[1] < 0 || t >= cl.len) continue;
      g.fillRect(x + Math.max(0, t) * ppt, y + h - ((n[2] - lo + 1) / range) * h, Math.max(1, n[1] * ppt - 1), Math.max(1.5, h / range - 0.5));
    }
}

function drawAutoPreview(g: CanvasRenderingContext2D, pr: Project, cl: Clip, x: number, y: number, h: number, ppt: number, color: string) {
  const a = pr.automation.find((p) => p.id === cl.ref);
  if (!a || !a.points.length) return;
  g.strokeStyle = color;
  g.lineWidth = 1.5;
  g.beginPath();
  a.points.forEach((p, i) => {
    const px = x + (p[0] - cl.offset) * ppt, py = y + h - p[1] * h;
    if (i === 0) g.moveTo(Math.min(px, x), py);
    g.lineTo(px, py);
  });
  const last = a.points[a.points.length - 1];
  g.lineTo(x + cl.len * ppt, y + h - last[1] * h);
  g.stroke();
}
