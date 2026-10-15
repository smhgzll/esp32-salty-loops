// SPDX-License-Identifier: GPL-3.0-or-later
// Piano-roll editing operations on note lists (pure functions: tested in model.test.ts).
import type { Note } from '../app/types';

export const SNAPS = [
  { label: 'Bar', ticks: -1 }, // the time signature's bar
  { label: 'Beat', ticks: 960 },
  { label: '1/2 beat', ticks: 480 },
  { label: 'Step', ticks: 240 },
  { label: '1/2 step', ticks: 120 },
  { label: '1/3 beat', ticks: 320 },
  { label: '1/6 beat', ticks: 160 },
  { label: 'None', ticks: 1 },
];

export const snapDown = (t: number, s: number) => (s <= 1 ? Math.round(t) : Math.floor(t / s) * s);
export const snapNear = (t: number, s: number) => (s <= 1 ? Math.round(t) : Math.round(t / s) * s);

export function nextId(notes: Note[]) {
  let m = 0;
  for (const n of notes) m = Math.max(m, n.id);
  return m + 1;
}

export function newNote(id: number, start: number, len: number, key: number, vel = 0.78): Note {
  return { id, start, len, key, vel, pan: 0, pitch: 0, rel: 0.5, prob: 1, slide: false, accent: false };
}

const sorted = (ns: Note[]) => [...ns].sort((a, b) => a.start - b.start || a.key - b.key);

/** move notes (by id) by dt ticks and dk keys, clamped to the valid range */
export function moveNotes(ns: Note[], ids: Set<number>, dt: number, dk: number): Note[] {
  let minStart = Infinity, minKey = 127, maxKey = 0;
  for (const n of ns)
    if (ids.has(n.id)) {
      minStart = Math.min(minStart, n.start);
      minKey = Math.min(minKey, n.key);
      maxKey = Math.max(maxKey, n.key);
    }
  if (minStart + dt < 0) dt = -minStart;
  if (minKey + dk < 0) dk = -minKey;
  if (maxKey + dk > 127) dk = 127 - maxKey;
  return sorted(ns.map((n) => (ids.has(n.id) ? { ...n, start: n.start + dt, key: n.key + dk } : n)));
}

/** resize notes' ends by dl ticks (minimum length `min`) */
export function resizeNotes(ns: Note[], ids: Set<number>, dl: number, min: number): Note[] {
  return ns.map((n) => (ids.has(n.id) ? { ...n, len: Math.max(min, n.len + dl) } : n));
}

export function deleteNotes(ns: Note[], ids: Set<number>) {
  return ns.filter((n) => !ids.has(n.id));
}

/** quantize starts (and optionally lengths) of the given notes (all when ids is empty) */
export function quantize(ns: Note[], ids: Set<number>, grid: number, lengths = false): Note[] {
  const all = ids.size === 0;
  return sorted(
    ns.map((n) => {
      if (!all && !ids.has(n.id)) return n;
      const start = Math.max(0, snapNear(n.start, grid));
      const len = lengths ? Math.max(grid, snapNear(n.len, grid)) : n.len;
      return { ...n, start, len };
    }),
  );
}

export function transpose(ns: Note[], ids: Set<number>, semis: number): Note[] {
  const all = ids.size === 0;
  return ns.map((n) => ((all || ids.has(n.id)) ? { ...n, key: Math.max(0, Math.min(127, n.key + semis)) } : n));
}

/** every note lasts until the next note start (per pitch-independent timeline), like a legato tool */
export function legato(ns: Note[], ids: Set<number>): Note[] {
  const all = ids.size === 0;
  const s = sorted(ns);
  const starts = [...new Set(s.map((n) => n.start))].sort((a, b) => a - b);
  return s.map((n) => {
    if (!all && !ids.has(n.id)) return n;
    const next = starts.find((t) => t > n.start);
    return next === undefined ? n : { ...n, len: next - n.start };
  });
}

/** cut notes into pieces of `grid` ticks */
export function chop(ns: Note[], ids: Set<number>, grid: number): Note[] {
  const all = ids.size === 0;
  let id = nextId(ns);
  const out: Note[] = [];
  for (const n of ns) {
    if ((!all && !ids.has(n.id)) || n.len <= grid) {
      out.push(n);
      continue;
    }
    for (let t = 0; t < n.len; t += grid) out.push({ ...n, id: t === 0 ? n.id : id++, start: n.start + t, len: Math.min(grid, n.len - t) });
  }
  return sorted(out);
}

/** copies of the notes placed `offset` ticks later, with fresh ids; returns [all, new ids] */
export function duplicate(ns: Note[], ids: Set<number>, offset: number): [Note[], Set<number>] {
  let id = nextId(ns);
  const add: Note[] = [];
  for (const n of ns) if (ids.has(n.id)) add.push({ ...n, id: id++, start: n.start + offset });
  return [sorted([...ns, ...add]), new Set(add.map((n) => n.id))];
}

/** span of a selection: [start, end) */
export function span(ns: Note[], ids: Set<number>): [number, number] {
  let a = Infinity, b = -Infinity;
  for (const n of ns)
    if (ids.has(n.id)) {
      a = Math.min(a, n.start);
      b = Math.max(b, n.start + n.len);
    }
  return [a, b];
}

/** paste clipboard notes (relative to their first start) at `at` */
export function paste(ns: Note[], clip: Note[], at: number): [Note[], Set<number>] {
  if (!clip.length) return [ns, new Set()];
  const first = Math.min(...clip.map((n) => n.start));
  let id = nextId(ns);
  const add = clip.map((n) => ({ ...n, id: id++, start: n.start - first + at }));
  return [sorted([...ns, ...add]), new Set(add.map((n) => n.id))];
}

export function randomizeVelocity(ns: Note[], ids: Set<number>, amount: number, rnd = Math.random): Note[] {
  const all = ids.size === 0;
  return ns.map((n) => ((all || ids.has(n.id)) ? { ...n, vel: Math.max(0.05, Math.min(1, n.vel + (rnd() * 2 - 1) * amount)) } : n));
}

export function inRect(n: Note, t0: number, t1: number, k0: number, k1: number) {
  return n.start < t1 && n.start + n.len > t0 && n.key >= k0 && n.key <= k1;
}
