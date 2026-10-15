// SPDX-License-Identifier: GPL-3.0-or-later
// Automation clip editor: the clip's points over its length, values 0..1 mapped to the target's range
// exactly as the engine maps them (engine.cpp mapValue / automate: linear, or logarithmic for log-scale
// parameters; a point's tension bends the segment after it: f^(2^(3 * tension))).
//   click empty space: add a point          drag a point: move it (between its neighbours)
//   right-click / long-press a point: delete    drag a segment vertically, or alt-drag: tension
//   double-click a point: reset its tension
// Every gesture is one undo step (auto.set with the whole point list).
import { useEffect, useRef, useState } from 'preact/hooks';

import * as A from '../app/actions';
import { catalog, proj, ui } from '../app/state';
import { useStore } from '../app/store';
import type { Automation, ParamDesc } from '../app/types';
import { clamp, dbText, rgba } from '../app/util';
import { Btn, Modal, showMenu } from '../ui/controls';
import { fmtParam } from '../plugins/PluginEditor';

type Pt = [number, number, number];
const PAD = { l: 52, r: 14, t: 14, b: 26 };
const HIT = 9;

export const openAutomation = (id: number) => ui.set({ autoEdit: id });

/** the curve the engine plays: value at local tick t */
export function valueAt(pts: Pt[], t: number) {
  if (!pts.length) return 0.5;
  if (t <= pts[0][0]) return pts[0][1];
  const last = pts[pts.length - 1];
  if (t >= last[0]) return last[1];
  let i = 1;
  while (i < pts.length && pts[i][0] <= t) i++;
  const [t0, v0, k] = pts[i - 1], [t1, v1] = pts[i];
  let f = (t - t0) / (t1 - t0);
  if (k) f = Math.pow(f, Math.pow(2, k * 3));
  return v0 + (v1 - v0) * f;
}

/** the target's range and a formatter for a normalised value */
function targetFormat(a: Automation): (v: number) => string {
  const p = proj.state.project;
  const t = a.target;
  const lin = (lo: number, hi: number) => (v: number) => lo + (hi - lo) * v;
  switch (t.kind) {
    case 'channelVol':
    case 'trackVol':
    case 'sendLevel':
      return (v) => `${dbText(lin(0, 1.25)(v))} dB`;
    case 'channelPan':
    case 'trackPan': {
      return (v) => {
        const x = Math.round(lin(-1, 1)(v) * 100);
        return x === 0 ? 'C' : x < 0 ? `${-x} L` : `${x} R`;
      };
    }
    case 'fxMix':
      return (v) => `${Math.round(v * 100)} % wet`;
    case 'tempo':
      return (v) => `${lin(20, 300)(v).toFixed(1)} BPM`;
    case 'channelParam':
    case 'fxParam': {
      let pluginId = '';
      if (t.kind === 'channelParam') pluginId = p?.channels.find((c) => c.id === t.channel)?.plugin.id ?? '';
      else for (const tr of p?.mixer ?? []) for (const f of tr.fx) if (f.id === t.fx) pluginId = f.plugin.id;
      const d: ParamDesc | undefined = catalog.state.plugins.find((x) => x.id === pluginId)?.params.find((x) => x.key === t.param);
      if (!d) return (v) => v.toFixed(2);
      return (v) => fmtParam(d, d.scale === 'log' && d.min > 0 ? d.min * Math.pow(d.max / d.min, v) : lin(d.min, d.max)(v));
    }
    default:
      return (v) => v.toFixed(2);
  }
}

const LENGTHS = [1, 2, 4, 8, 16, 32];

export function AutomationEditor() {
  const id = useStore(ui, (s) => s.autoEdit);
  const auto = useStore(proj, (s) => s.project?.automation.find((x) => x.id === id));
  const bar = useStore(proj, (s) => (s.project ? (960 * 4 * s.project.meta.timeSig[0]) / s.project.meta.timeSig[1] : 3840));
  const close = () => ui.set({ autoEdit: 0 });
  if (!id) return null;
  if (!auto)
    return (
      <Modal title="Automation" onClose={close}>
        <div class="hint">This automation clip no longer exists.</div>
      </Modal>
    );
  return (
    <Modal title={`Automation · ${auto.name}`} onClose={close} wide>
      <Editor a={auto} bar={bar} />
    </Modal>
  );
}

function Editor(p: { a: Automation; bar: number }) {
  const { a, bar } = p;
  const cv = useRef<HTMLCanvasElement>(null);
  const [snap, setSnap] = useState(240);
  const [, redraw] = useState(0);
  const S = useRef({
    pts: a.points.map((x) => [...x] as Pt),
    g: null as null | { kind: 'move' | 'tension'; i: number; before: Pt[]; y0: number; k0: number; moved: boolean },
    lp: null as ReturnType<typeof setTimeout> | null,
    lastTap: { i: -1, t: 0 },
    w: 0,
    h: 0,
  }).current;
  // the device's points, unless a gesture is editing them
  if (!S.g) S.pts = a.points.map((x) => [...x] as Pt);
  const len = a.length;
  const fmt = targetFormat(a);

  const xOf = (t: number) => PAD.l + (t / len) * (S.w - PAD.l - PAD.r);
  const tOf = (x: number) => clamp(((x - PAD.l) / (S.w - PAD.l - PAD.r)) * len, 0, len);
  const yOf = (v: number) => PAD.t + (1 - v) * (S.h - PAD.t - PAD.b);
  const vOf = (y: number) => clamp(1 - (y - PAD.t) / (S.h - PAD.t - PAD.b), 0, 1);
  const snapT = (t: number) => (snap > 1 ? Math.round(t / snap) * snap : Math.round(t));

  const draw = () => {
    const c = cv.current;
    if (!c) return;
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth, h = c.clientHeight;
    S.w = w;
    S.h = h;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    const g = c.getContext('2d')!;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    g.fillStyle = '#1c2024';
    g.fillRect(PAD.l, PAD.t, w - PAD.l - PAD.r, h - PAD.t - PAD.b);
    // grid: beats and bars, value quarters
    const beat = bar / 4;
    for (let t = 0; t <= len; t += beat) {
      const x = Math.round(xOf(t)) + 0.5;
      g.strokeStyle = t % bar === 0 ? '#4a535c' : '#2c3238';
      g.beginPath();
      g.moveTo(x, PAD.t);
      g.lineTo(x, h - PAD.b);
      g.stroke();
      if (t % bar === 0 && t < len) {
        g.fillStyle = '#9aa4ad';
        g.font = '11px system-ui, sans-serif';
        g.fillText(String(t / bar + 1), x + 3, h - 8);
      }
    }
    g.font = '10px system-ui, sans-serif';
    for (const v of [0, 0.25, 0.5, 0.75, 1]) {
      const y = Math.round(yOf(v)) + 0.5;
      g.strokeStyle = '#2c3238';
      g.beginPath();
      g.moveTo(PAD.l, y);
      g.lineTo(w - PAD.r, y);
      g.stroke();
      g.fillStyle = '#8a949d';
      g.textAlign = 'right';
      g.fillText(fmt(v), PAD.l - 4, y + 3);
      g.textAlign = 'left';
    }
    // the curve, sampled like the engine evaluates it
    const col = a.color || '#40c4aa';
    g.beginPath();
    const steps = Math.max(64, Math.round(w - PAD.l - PAD.r));
    for (let i = 0; i <= steps; i++) {
      const t = (i / steps) * len;
      const x = xOf(t), y = yOf(valueAt(S.pts, t));
      if (i === 0) g.moveTo(x, y);
      else g.lineTo(x, y);
    }
    g.strokeStyle = col;
    g.lineWidth = 2;
    g.stroke();
    g.lineTo(xOf(len), yOf(0));
    g.lineTo(xOf(0), yOf(0));
    g.closePath();
    g.fillStyle = rgba(col, 0.15);
    g.fill();
    g.lineWidth = 1;
    // points
    S.pts.forEach(([t, v, k], i) => {
      const x = xOf(t), y = yOf(v);
      g.beginPath();
      g.arc(x, y, i === S.g?.i ? 6 : 4.5, 0, Math.PI * 2);
      g.fillStyle = k ? '#ffb03a' : '#e8edf1';
      g.fill();
      g.strokeStyle = '#000';
      g.stroke();
    });
    if (S.g) {
      const pt = S.pts[S.g.i];
      if (pt) {
        g.fillStyle = '#fff';
        g.font = '11px system-ui, sans-serif';
        const label = S.g.kind === 'tension' ? `tension ${pt[2].toFixed(2)}` : `${fmt(pt[1])} @ bar ${(pt[0] / bar + 1).toFixed(2)}`;
        g.fillText(label, clamp(xOf(pt[0]) + 8, PAD.l, w - 140), clamp(yOf(pt[1]) - 8, 12, h - PAD.b));
      }
    }
  };
  const latestDraw = useRef(draw);
  latestDraw.current = draw;
  useEffect(() => {
    const c = cv.current!;
    const ro = new ResizeObserver(() => latestDraw.current());
    ro.observe(c);
    return () => ro.disconnect();
  }, []);
  useEffect(draw);

  const local = (e: PointerEvent) => {
    const r = cv.current!.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };
  const pointAt = (x: number, y: number) => {
    let best = -1, bd = HIT * HIT;
    S.pts.forEach(([t, v], i) => {
      const d = (xOf(t) - x) ** 2 + (yOf(v) - y) ** 2;
      if (d <= bd) (bd = d), (best = i);
    });
    return best;
  };
  const commit = (label: string, before: Pt[]) => A.setAutomationPoints(a.id, S.pts.map((x) => [...x] as Pt), label, before);
  const remove = (i: number) => {
    if (S.pts.length <= 1) return;
    const before = S.pts.map((x) => [...x] as Pt);
    S.pts = S.pts.filter((_, k) => k !== i);
    commit('Delete automation point', before);
  };

  const onDown = (e: PointerEvent) => {
    const { x, y } = local(e);
    if (x < PAD.l - 4 || y > S.h - PAD.b + 4) return;
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    const i = pointAt(x, y);
    if (e.button === 2) {
      if (i >= 0) remove(i);
      return;
    }
    const before = S.pts.map((q) => [...q] as Pt);
    if (i >= 0) {
      const now = Date.now();
      if (S.lastTap.i === i && now - S.lastTap.t < 320 && S.pts[i][2]) {
        S.pts[i][2] = 0;
        commit('Reset tension', before);
        S.lastTap = { i: -1, t: 0 };
        return;
      }
      S.lastTap = { i, t: now };
      S.g = { kind: e.altKey ? 'tension' : 'move', i, before, y0: y, k0: S.pts[i][2], moved: false };
      if (e.pointerType === 'touch')
        S.lp = setTimeout(() => {
          S.lp = null;
          if (S.g && !S.g.moved) {
            S.pts = S.g.before;
            S.g = null;
            showMenu(
              [
                { label: 'Delete point', danger: true, disabled: S.pts.length <= 1, action: () => remove(i) },
                { label: 'Reset tension', disabled: !S.pts[i]?.[2], action: () => { const b = S.pts.map((q) => [...q] as Pt); S.pts[i][2] = 0; commit('Reset tension', b); } },
              ],
              e.clientX,
              e.clientY,
            );
            draw();
          }
        }, 520);
      draw();
      return;
    }
    // on a segment (close to the curve): bend it
    const t = tOf(x);
    const seg = S.pts.findIndex((q, k) => k < S.pts.length - 1 && t > q[0] && t < S.pts[k + 1][0]);
    if (seg >= 0 && (e.altKey || Math.abs(yOf(valueAt(S.pts, t)) - y) < 8)) {
      S.g = { kind: 'tension', i: seg, before, y0: y, k0: S.pts[seg][2], moved: false };
      draw();
      return;
    }
    // empty space: a new point, then dragged
    if (S.pts.length >= 1024) return;
    const np: Pt = [snapT(t), vOf(y), 0];
    S.pts = [...S.pts, np].sort((p1, p2) => p1[0] - p2[0]);
    S.g = { kind: 'move', i: S.pts.indexOf(np), before, y0: y, k0: 0, moved: true };
    draw();
  };
  const onMove = (e: PointerEvent) => {
    const gs = S.g;
    if (!gs) return;
    const { x, y } = local(e);
    if (S.lp && Math.abs(y - gs.y0) > 6) {
      clearTimeout(S.lp);
      S.lp = null;
    }
    gs.moved = true;
    const pt = S.pts[gs.i];
    if (gs.kind === 'tension') {
      // up = faster start (negative tension); the sign follows the segment's direction
      const rising = S.pts[gs.i + 1] ? S.pts[gs.i + 1][1] >= pt[1] : true;
      pt[2] = clamp(gs.k0 + ((y - gs.y0) / 120) * (rising ? 1 : -1), -1, 1);
    } else {
      const lo = gs.i > 0 ? S.pts[gs.i - 1][0] : 0, hi = gs.i < S.pts.length - 1 ? S.pts[gs.i + 1][0] : len;
      pt[0] = clamp(snapT(tOf(x)), lo, hi);
      pt[1] = e.shiftKey ? pt[1] : vOf(y);
    }
    draw();
  };
  const onUp = () => {
    if (S.lp) {
      clearTimeout(S.lp);
      S.lp = null;
    }
    const gs = S.g;
    S.g = null;
    if (gs && gs.moved) commit(gs.kind === 'tension' ? 'Automation curve' : 'Edit automation', gs.before);
    draw();
  };

  const lenBars = Math.round(len / bar);
  return (
    <div class="auto-ed">
      <div class="row" style={{ gap: '8px', marginBottom: '8px', flexWrap: 'wrap' }}>
        <span class="hint">Length</span>
        <select class="mini" aria-label="Automation length" value={lenBars} onChange={(e) => A.setAutomation(a.id, { length: +(e.target as HTMLSelectElement).value * bar })}>
          {(LENGTHS.includes(lenBars) ? LENGTHS : [...LENGTHS, lenBars].sort((x, y) => x - y)).map((n) => (
            <option key={n} value={n}>
              {n} {n === 1 ? 'bar' : 'bars'}
            </option>
          ))}
        </select>
        <span class="hint">Snap</span>
        <select class="mini" aria-label="Snap" value={snap} onChange={(e) => setSnap(+(e.target as HTMLSelectElement).value)}>
          <option value={1}>none</option>
          <option value={60}>1/64</option>
          <option value={120}>1/32</option>
          <option value={240}>step</option>
          <option value={960}>beat</option>
          <option value={bar}>bar</option>
        </select>
        <Btn onClick={() => { const n = prompt('Automation name', a.name); if (n) A.setAutomation(a.id, { name: n }); redraw((x) => x + 1); }}>Rename…</Btn>
        <span class="hint" style={{ whiteSpace: 'normal' }}>
          click: add · drag: move · right-click / long-press: delete · drag the line or alt-drag: curve · double-click: straight
        </span>
      </div>
      <canvas
        ref={cv}
        class="auto-cv"
        aria-label="Automation points"
        style={{ width: '100%', height: 'min(320px, 52vh)', touchAction: 'none', display: 'block' }}
        onPointerDown={onDown}
        onPointerMove={onMove}
        onPointerUp={onUp}
        onPointerCancel={onUp}
        onContextMenu={(e) => e.preventDefault()}
      />
    </div>
  );
}
