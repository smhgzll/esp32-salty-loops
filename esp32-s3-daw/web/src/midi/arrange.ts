// SPDX-License-Identifier: GPL-3.0-or-later
// From a parsed MIDI file to DAW material (pure functions, tested in arrange.test.ts):
//   arrange()          a song's parts as patterns and playlist clips. Each part is cut into
//                      segments of a few bars; a cut moves to a later (or earlier) bar line when a
//                      note would cross it, so no note is shortened. Segments with the same notes
//                      share one pattern, like a hand-made arrangement.
//   tempoAutomation()  the tempo map as tempo automation clip(s)
//   sliceNotes()       a range of some parts' notes, for the piano roll
import { GM_PROGRAMS } from './gm.ts';
import { PPQ } from './smf.ts';
import type { MidiNote, MidiPart, MidiSong } from './smf.ts';

/** the device's limits (components/daw_core: model.hpp and the session's validation) */
export const LIMITS = {
  channels: 32,
  patterns: 256,
  clips: 2048,
  notesPerPattern: 4096,
  playlistTracks: 64,
  autoPoints: 1024,
  patternTicks: PPQ * 4 * 64, // longest fixed pattern length (and note)
  noteStartTicks: PPQ * 4 * 256, // latest note start in a pattern
  clipTicks: PPQ * 4 * 1000, // latest clip start
  autoTicks: PPQ * 4 * 256, // longest automation clip
};

export interface PartNotes {
  name: string;
  notes: MidiNote[]; // sorted by start
}
export interface PatternPlan {
  part: number;
  name: string;
  length: number;
  notes: MidiNote[]; // starts relative to the pattern
}
export interface ClipPlan {
  part: number;
  pattern: number;
  start: number;
  len: number;
}
export interface Arrangement {
  patterns: PatternPlan[];
  clips: ClipPlan[];
  segmentBars: number; // the size used (larger than asked when the limits required it)
  truncated: number; // notes cut at a pattern end (only notes held over more than 64 bars)
  end: number;
}

export const barTicksOf = (num: number, den: number) => ((PPQ * 4) / den) * num;

/** the project time signature for a song: its first one when the device supports it, else 4/4 */
export function songTimeSig(song: MidiSong): { num: number; den: number; changes: number; supported: boolean } {
  const first = song.timeSigs[0];
  const at0 = first && first.tick === 0 ? first : undefined;
  const supported = !at0 || (at0.num >= 1 && at0.num <= 16 && [2, 4, 8, 16].includes(at0.den));
  return {
    num: at0 && supported ? at0.num : 4,
    den: at0 && supported ? at0.den : 4,
    changes: song.timeSigs.filter((x) => x.tick > 0).length,
    supported,
  };
}

const label = (k: number) => (k < 26 ? String.fromCharCode(65 + k) : String(k + 1));

/** the stretches of time some note holds, joined where notes overlap (not where they only touch) */
function held(notes: MidiNote[]): [number, number][] {
  const iv: [number, number][] = [];
  for (const n of notes) {
    const last = iv[iv.length - 1];
    if (last && n.start < last[1]) last[1] = Math.max(last[1], n.start + n.len);
    else iv.push([n.start, n.start + n.len]);
  }
  return iv;
}

interface Segment {
  start: number;
  len: number;
  notes: MidiNote[];
}

function segmentPart(notes: MidiNote[], barTicks: number, segTicks: number, maxTicks: number) {
  const iv = held(notes);
  // does a note sound across tick b (start < b < end)?
  const crosses = (b: number) => {
    let lo = 0, hi = iv.length;
    while (lo < hi) {
      const m = (lo + hi) >> 1;
      if (iv[m][0] < b) lo = m + 1;
      else hi = m;
    }
    return lo > 0 && b < iv[lo - 1][1];
  };
  const segs: Segment[] = [];
  let truncated = 0;
  let i = 0;
  let s = 0;
  while (i < notes.length) {
    const cell = Math.floor(notes[i].start / segTicks) * segTicks;
    if (cell > s) s = cell; // skip silence (back on the grid)
    let e = (Math.floor(s / segTicks) + 1) * segTicks;
    if (crosses(e)) {
      let f = -1;
      for (let b = e + barTicks; b - s <= maxTicks; b += barTicks)
        if (!crosses(b)) {
          f = b;
          break;
        }
      if (f < 0)
        for (let b = e - barTicks; b > s; b -= barTicks)
          if (!crosses(b)) {
            f = b;
            break;
          }
      if (f > 0) e = f;
    }
    const out: MidiNote[] = [];
    while (i < notes.length && notes[i].start < e) {
      const n = notes[i++];
      const len = Math.min(n.len, e - n.start);
      if (len < n.len) truncated++;
      out.push({ ...n, start: n.start - s, len });
    }
    if (out.length) segs.push({ start: s, len: e - s, notes: out });
    s = e;
  }
  return { segs, truncated };
}

function tryArrange(parts: PartNotes[], barTicks: number, bars: number, maxTicks: number): Arrangement {
  const patterns: PatternPlan[] = [];
  const clips: ClipPlan[] = [];
  let truncated = 0;
  let end = 0;
  parts.forEach((part, pi) => {
    if (!part.notes.length) return;
    const r = segmentPart(part.notes, barTicks, bars * barTicks, maxTicks);
    truncated += r.truncated;
    const seen = new Map<string, number>();
    for (const g of r.segs) {
      const key = `${g.len}|${g.notes.map((n) => `${n.start},${n.len},${n.key},${Math.round(n.vel * 127)},${n.rel ?? .5},${n.onOrder ?? 0},${n.offOrder ?? 0}`).join(';')}`;
      let idx = seen.get(key);
      if (idx === undefined) {
        if (g.notes.length > LIMITS.notesPerPattern)
          throw new Error(`${part.name}: ${g.notes.length} notes in ${g.len / barTicks} bars (a pattern holds ${LIMITS.notesPerPattern})`);
        idx = patterns.length;
        patterns.push({ part: pi, name: `${part.name} ${label(seen.size)}`.slice(0, 64), length: g.len, notes: g.notes });
        seen.set(key, idx);
      }
      clips.push({ part: pi, pattern: idx, start: g.start, len: g.len });
      end = Math.max(end, g.start + g.len);
    }
  });
  return { patterns, clips, segmentBars: bars, truncated, end };
}

/** the parts as patterns and clips, in segments of `segmentBars` bars, or larger ones when the
 *  device's pattern / clip limits need it; throws when even the largest do not fit */
export function arrange(parts: PartNotes[], barTicks: number, segmentBars: number, maxPatterns = LIMITS.patterns - 1): Arrangement {
  const maxBars = Math.max(1, Math.floor(LIMITS.patternTicks / barTicks));
  let end = 0;
  for (const p of parts) for (const n of p.notes) end = Math.max(end, n.start + n.len);
  if (end > LIMITS.clipTicks) throw new Error(`the song is longer than the playlist (${Math.ceil(end / barTicks)} bars; at most ${Math.floor(LIMITS.clipTicks / barTicks)})`);
  let bars = Math.max(1, Math.min(segmentBars, maxBars));
  for (;;) {
    const a = tryArrange(parts, barTicks, bars, maxBars * barTicks);
    if (a.patterns.length <= maxPatterns && a.clips.length <= LIMITS.clips) return a;
    if (bars >= maxBars)
      throw new Error(`too much material for the device: ${a.patterns.length} patterns and ${a.clips.length} clips even with ${bars}-bar patterns (it holds ${maxPatterns} and ${LIMITS.clips})`);
    bars = Math.min(bars * 2, maxBars);
  }
}

/** how the import maps parts to channels: one each, or the parts a GM synth plays with one
 *  instrument as one ('channel': the same MIDI channel, bank and program, like the separate drum
 *  tracks of a format 1 file; 'instrument': the same program on any channel and bank) */
export type MergeMode = 'part' | 'channel' | 'instrument';

/** the parts merged by `mode`: notes of all, sorted (an exact repeat of a key at the same tick is
 *  one note, the longer); velocities keep their loudness against the merged part's volume (the
 *  loudest); the name, pan and program of the part with the most notes */
export function mergeParts(parts: MidiPart[], mode: MergeMode): MidiPart[] {
  if (mode === 'part') return parts;
  const groups = new Map<string, MidiPart[]>();
  for (const p of parts) {
    // ('instrument' leaves the bank out: GS / XG variation banks fall back to the GM program)
    const k = `${p.port ?? 0}|` + (p.drums ? `d|${mode === 'channel' ? p.channel : p.program}` : mode === 'channel' ? `${p.channel}|${p.bank}|${p.program}` : `|${p.program}`);
    const stream = p.events?.some((e) => e.data[0] < 0xf0) ? `|midi:${p.channel}` : '';
    const g = groups.get(k + stream);
    if (g) g.push(p);
    else groups.set(k + stream, [p]);
  }
  return [...groups.values()].map((g) => {
    if (g.length === 1) return g[0];
    const main = g.reduce((a, b) => (b.notes.length > a.notes.length ? b : a));
    const volume = Math.max(...g.map((p) => p.volume));
    const all = g.flatMap((p) => p.notes.map((n) => ({ ...n, vel: Math.max(1 / 127, Math.min(1, Math.round(((n.vel * p.volume) / volume) * 10000) / 10000)) })));
    all.sort((a, b) => a.start - b.start || a.key - b.key || b.len - a.len);
    const notes = all.filter((n, i) => i === 0 || n.start !== all[i - 1].start || n.key !== all[i - 1].key);
    const names = new Set(g.map((p) => p.name));
    const name = names.size === 1 ? main.name : main.drums ? 'Drums' : main.program >= 0 ? GM_PROGRAMS[main.program] : main.name;
    const events = [...new Map(g.flatMap((p) => p.events ?? []).map((e) => [JSON.stringify([e.tick, e.order, e.data]), e])).values()].sort((a, b) => a.tick - b.tick || a.order - b.order);
    return { ...main, name, notes, volume, events, programChanges: Math.max(...g.map((p) => p.programChanges)) };
  });
}

export interface AutoPlan {
  start: number;
  length: number;
  points: [number, number, number][]; // [tick, value 0..1, tension]
}
/** tempo automation's normalised value (the device maps 0..1 to 20..300 BPM) */
export const tempoValue = (bpm: number) => Math.min(1, Math.max(0, (bpm - 20) / 280));

/** the tempo map as automation clips (none when the tempo never changes); steps, at most
 *  LIMITS.autoPoints points and LIMITS.autoTicks ticks per clip */
export function tempoAutomation(tempos: { tick: number; bpm: number }[], end: number): AutoPlan[] {
  if (tempos.length < 2 || end <= 0) return [];
  const out: AutoPlan[] = [];
  const at = (t: number) => {
    let v = tempos[0].bpm;
    for (const x of tempos) if (x.tick <= t) v = x.bpm;
    return v;
  };
  const maxChanges = (LIMITS.autoPoints - 2) >> 1;
  for (let s = 0; s < end;) {
    let finish = Math.min(end, s + LIMITS.autoTicks);
    let changes = tempos.filter((x) => x.tick > s && x.tick < finish);
    if (changes.length > maxChanges) {
      finish = changes[maxChanges].tick;
      if (finish - s < PPQ) throw new Error('more than 511 tempo changes in one beat cannot fit the device automation');
      changes = changes.slice(0, maxChanges);
    }
    const length = Math.max(PPQ, finish - s);
    let prev = tempoValue(at(s));
    const points: [number, number, number][] = [[0, prev, 0]];
    for (const c of changes) {
      const v = tempoValue(c.bpm);
      points.push([c.tick - s, prev, 0], [c.tick - s, v, 0]);
      prev = v;
    }
    points.push([length, prev, 0]);
    out.push({ start: s, length, points });
    s = finish;
  }
  return out;
}

/** notes of some parts that start in [from, to), moved to start at 0 and cut at `to`; `map`
 *  changes keys (null drops the note) */
export function sliceNotes(lists: MidiNote[][], from: number, to: number, map?: (key: number) => number | null) {
  const notes: MidiNote[] = [];
  let clipped = 0;
  let dropped = 0;
  for (const list of lists)
    for (const n of list) {
      if (n.start < from || n.start >= to) continue;
      const key = map ? map(n.key) : n.key;
      if (key === null) {
        dropped++;
        continue;
      }
      const len = Math.min(n.len, to - n.start, LIMITS.patternTicks);
      if (len < n.len) clipped++;
      notes.push({ ...n, start: n.start - from, len, key });
    }
  notes.sort((a, b) => a.start - b.start || a.key - b.key);
  return { notes, clipped, dropped };
}

/** what a document of `json` bytes takes in a slot that stores it deflated: imported projects
 *  measured 5.3-11 times smaller (2026-10-13, 5 songs); a quarter keeps the estimate on the safe side */
export const estimateStoredBytes = (json: number) => Math.ceil(json / 4) + 512;

/** the saved project's approximate size in bytes (the device keeps projects as JSON; a pattern
 *  holds only its part's row, notes without their default fields: project_json.cpp) */
export function estimateProjectBytes(a: Arrangement, channels: number, autos: AutoPlan[], markers: number) {
  let b = 7000 + channels * 2600; // document, mixer and playlist tracks; a channel with its plugin's parameters
  for (const p of a.patterns) {
    b += 200; // the pattern and its part's row
    for (let i = 0; i < p.notes.length; ++i) {
      const n = p.notes[i];
      const fields = [i + 1, n.start, n.len, n.key, Math.round(n.vel * 10000) / 10000, 0, 0, Math.round((n.rel ?? .5) * 10000) / 10000, 1, 0, n.onOrder ?? 0, n.offOrder ?? 0];
      const defaults = [0, 0, 0, 0, 0, 0, 0, .5, 1, 0, 0, 0];
      while (fields.length > 5 && fields[fields.length - 1] === defaults[fields.length - 1]) fields.pop();
      b += JSON.stringify(fields).length + 1;
    }
  }
  b += a.clips.length * 75 + markers * 45;
  for (const x of autos) b += 250 + x.points.length * 20;
  return b;
}
