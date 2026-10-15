// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI file -> patterns, clips and automation (arrange.ts): run with `npm test` in web/.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { LIMITS, arrange, mergeParts, sliceNotes, tempoAutomation, tempoValue } from './arrange.ts';
import type { Arrangement, PartNotes } from './arrange.ts';
import { drumLabKey } from './gm.ts';
import type { MidiNote } from './smf.ts';

const BAR = 3840; // 4/4 at 960 PPQ
const n = (start: number, len: number, key = 60, vel = 0.8): MidiNote => ({ start, len, key, vel });

/** the notes the playlist plays: every clip's pattern at the clip's position */
function played(a: Arrangement, part: number) {
  const out: [number, number, number][] = [];
  for (const c of a.clips) if (c.part === part) for (const x of a.patterns[c.pattern].notes) out.push([c.start + x.start, x.len, x.key]);
  return out.sort((p, q) => p[0] - q[0] || p[2] - q[2]);
}
const flat = (ns: MidiNote[]) => ns.map((x) => [x.start, x.len, x.key] as [number, number, number]).sort((p, q) => p[0] - q[0] || p[2] - q[2]);

test('a riff repeated four times becomes one pattern and four clips', () => {
  const riff = [n(0, 960, 60), n(960, 960, 63), n(1920, 1920, 67)];
  const notes: MidiNote[] = [];
  for (let k = 0; k < 16; k++) for (const x of riff) notes.push({ ...x, start: x.start + k * BAR });
  const a = arrange([{ name: 'Lead', notes }], BAR, 4);
  assert.equal(a.patterns.length, 1);
  assert.equal(a.patterns[0].name, 'Lead A');
  assert.equal(a.patterns[0].length, 4 * BAR);
  assert.deepEqual(a.clips.map((c) => c.start), [0, 4, 8, 12].map((b) => b * BAR));
  assert.deepEqual(played(a, 0), flat(notes));
});

test('a cut moves to a bar line no note crosses; nothing is shortened', () => {
  // a pad held from bar 3 to bar 6 crosses the 4-bar line
  const notes = [n(0, BAR, 48), n(3 * BAR, 3 * BAR, 50), n(6 * BAR, BAR, 52), n(9 * BAR, BAR, 53)];
  const a = arrange([{ name: 'Pad', notes }], BAR, 4);
  assert.equal(a.truncated, 0);
  assert.deepEqual(a.clips.map((c) => [c.start / BAR, c.len / BAR]), [[0, 6], [6, 2], [8, 4]]);
  assert.deepEqual(played(a, 0), flat(notes));
  for (const p of a.patterns) for (const x of p.notes) assert.ok(x.start + x.len <= p.length, `${p.name} keeps its notes inside`);
});

test('silence is skipped: no clips for empty stretches', () => {
  const a = arrange([{ name: 'Hit', notes: [n(0, 240), n(40 * BAR, 240)] }], BAR, 4);
  assert.deepEqual(a.clips.map((c) => c.start / BAR), [0, 40]);
  assert.equal(a.patterns.length, 1);
});

test('random songs play back exactly (every part, any segment size)', () => {
  let seed = 7;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) / 16777216;
  for (let round = 0; round < 60; round++) {
    const parts: PartNotes[] = [];
    for (let p = 0; p < 3; p++) {
      const notes: MidiNote[] = [];
      let t = Math.floor(rnd() * 4) * 240;
      for (let i = 0; i < 80; i++) {
        notes.push(n(t, 30 + Math.floor(rnd() * (rnd() < 0.1 ? 6 * BAR : 1000)), 30 + Math.floor(rnd() * 60)));
        t += Math.floor(rnd() * 1200);
      }
      notes.sort((x, y) => x.start - y.start || x.key - y.key);
      parts.push({ name: `P${p}`, notes });
    }
    const a = arrange(parts, BAR, [1, 2, 4, 8][round % 4]);
    assert.equal(a.truncated, 0);
    parts.forEach((p, i) => assert.deepEqual(played(a, i), flat(p.notes)));
    for (const c of a.clips) assert.ok(c.start % BAR === 0 && c.len % BAR === 0 && c.len <= LIMITS.patternTicks);
  }
});

test('more patterns than the device holds: larger segments; impossible: an error', () => {
  // 300 different bars
  const notes = Array.from({ length: 300 }, (_, i) => n(i * BAR, 240, 20 + (i % 100), 0.2 + (i % 7) / 10));
  const a = arrange([{ name: 'X', notes }], BAR, 1);
  assert.ok(a.patterns.length <= LIMITS.patterns - 1);
  assert.ok(a.segmentBars > 1);
  assert.deepEqual(played(a, 0), flat(notes));
  let seed = 99;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) / 16777216;
  const many = Array.from({ length: 20 }, (_, p) => ({ name: `P${p}`, notes: Array.from({ length: 900 }, (_, i) => n(i * BAR, 240, Math.floor(rnd() * 128))) }));
  assert.throws(() => arrange(many, BAR, 4), /too much material|longer than/);
});

test('tempo map -> automation steps (none for a constant tempo)', () => {
  assert.deepEqual(tempoAutomation([{ tick: 0, bpm: 120 }], 10 * BAR), []);
  const a = tempoAutomation([{ tick: 0, bpm: 120 }, { tick: 4 * BAR, bpm: 90 }], 8 * BAR);
  assert.equal(a.length, 1);
  assert.deepEqual(a[0].points, [[0, tempoValue(120), 0], [4 * BAR, tempoValue(120), 0], [4 * BAR, tempoValue(90), 0], [8 * BAR, tempoValue(90), 0]]);
  assert.ok(Math.abs(20 + 280 * tempoValue(90) - 90) < 1e-9);
  // a long song: several clips, each within the limits
  const long = tempoAutomation(
    Array.from({ length: 3000 }, (_, i) => ({ tick: i * 960, bpm: 100 + (i % 20) })),
    700 * BAR,
  );
  assert.equal(long.length, 6); // keep every change, splitting at the point capacity
  for (const c of long) assert.ok(c.points.length <= LIMITS.autoPoints && c.length <= LIMITS.autoTicks);
});

test('piano roll slice: range, cut at its end, GM drums onto DrumLab', () => {
  const a = [n(0, 960, 36), n(BAR, 2 * BAR, 49), n(2 * BAR, 960, 71)];
  const b = [n(BAR + 480, 960, 38)];
  const s = sliceNotes([a, b], BAR, 3 * BAR - 960, drumLabKey);
  assert.deepEqual(flat(s.notes), [[0, 2 * BAR - 960, 46], [480, 960, 38]]); // crash -> open hat, cut
  assert.equal(s.clipped, 1);
  assert.equal(s.dropped, 1); // key 71 (whistle) has no DrumLab piece
});

test('merging parts: the drum tracks of a format 1 file and doubled instruments become one channel', () => {
  const part = (track: number, channel: number, program: number, notes: [number, number, number, number][], volume = 1, name = `T${track}`) => ({
    track, channel, name, trackName: name, instrument: '', program, bank: 0, drums: channel === 9, volume, pan: 0, programChanges: 0,
    notes: notes.map(([start, len, key, vel]) => ({ start, len, key, vel })),
  });
  const parts = [
    part(1, 9, 0, [[0, 30, 36, 1], [960, 30, 36, 1]], 1, 'Kick'),
    part(2, 9, 0, [[480, 30, 38, 0.5], [960, 60, 36, 1]], 0.5, 'Snare'), // the kick at 960 repeats track 1's
    part(3, 4, 30, [[0, 960, 40, 0.8]]),
    part(4, 6, 30, [[0, 960, 47, 0.8]]),
    part(5, 4, 29, [[0, 960, 52, 0.8]]),
  ];
  assert.equal(mergeParts(parts, 'part'), parts);
  const ch = mergeParts(parts, 'channel');
  assert.deepEqual(ch.map((p) => p.notes.length), [3, 1, 1, 1]); // drums merged, guitars on 4 differ in program
  assert.equal(ch[0].name, 'Drums');
  // a quieter track's notes stay quieter in the merged part; the repeated kick is one note, the longer
  assert.deepEqual(ch[0].notes.map((n) => [n.start, n.len, n.key, n.vel]), [[0, 30, 36, 1], [480, 30, 38, 0.25], [960, 60, 36, 0.5]]);
  const ins = mergeParts(parts, 'instrument');
  assert.deepEqual(ins.map((p) => [p.program, p.notes.length]), [[0, 3], [30, 2], [29, 1]]);
  assert.equal(ins[1].name, 'Distortion Guitar');
});
