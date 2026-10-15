// SPDX-License-Identifier: GPL-3.0-or-later
// Standard MIDI File reader (smf.ts): run with `npm test` in web/ (node --test).
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { MIN_NOTE_TICKS, MidiError, parseMidi } from './smf.ts';
import { bend, cc, encodeMidi, encodeTrack, marker, note, off, on, prog, sysex, tempo, timeSig, trackName } from './testing.ts';

const notesOf = (song: ReturnType<typeof parseMidi>, i: number) => song.parts[i].notes.map((n) => [n.start, n.len, n.key]);

test('format 1: tempo track, named parts, ticks converted to 960 PPQ', () => {
  const f = encodeMidi(1, 120, [
    [trackName(0, 'My Song'), tempo(0, 90), timeSig(0, 3, 4), marker(360, 'Verse')],
    [trackName(0, 'Piano:'), prog(0, 0, 4), ...note(0, 120, 0, 60), ...note(120, 60, 0, 64, 64)],
    [trackName(0, 'Bass'), prog(0, 1, 33), ...note(0, 360, 1, 36)],
  ]);
  const s = parseMidi(f);
  assert.equal(s.format, 1);
  assert.equal(s.title, 'My Song');
  assert.deepEqual(s.tempos, [{ tick: 0, bpm: 90 }]);
  assert.deepEqual(s.timeSigs, [{ tick: 0, num: 3, den: 4 }]);
  assert.deepEqual(s.markers, [{ tick: 2880, name: 'Verse' }]);
  assert.equal(s.parts.length, 2);
  assert.equal(s.parts[0].name, 'Piano'); // trailing colon removed
  assert.equal(s.parts[0].program, 4);
  assert.deepEqual(notesOf(s, 0), [[0, 960, 60], [960, 480, 64]]);
  assert.ok(Math.abs(s.parts[0].notes[1].vel - 64 / 127) < 1e-3);
  assert.deepEqual(notesOf(s, 1), [[0, 2880, 36]]);
  assert.equal(s.end, 2880);
});

test('format 0: one track, a part per MIDI channel, GM names and drums', () => {
  const f = encodeMidi(0, 96, [[prog(0, 0, 0), prog(0, 2, 40), ...note(0, 96, 0, 60), ...note(0, 96, 2, 72), ...note(48, 24, 9, 36)]]);
  const s = parseMidi(f);
  assert.equal(s.format, 0);
  assert.deepEqual(s.parts.map((p) => [p.channel, p.name, p.drums]), [[0, 'Acoustic Grand Piano', false], [2, 'Violin', false], [9, 'Drums', true]]);
  assert.deepEqual(notesOf(s, 2), [[480, 240, 36]]);
});

test('running status and note on with velocity 0 as the note off', () => {
  const evs = [on(0, 0, 60), on(0, 0, 64), off(96, 0, 60, true), off(96, 0, 64, true)];
  assert.equal(encodeTrack(evs).length, 4 + 3 + 3 + 3 + 4); // one status byte for all four events
  const s = parseMidi(encodeMidi(0, 96, [evs]));
  assert.deepEqual(notesOf(s, 0), [[0, 960, 60], [0, 960, 64]]);
});

test('SysEx events are retained whole, including long unknown vendor messages', () => {
  // GM System On, a GS reset and a long XG bulk message, then running status across them
  const gmOn = [0x7e, 0x7f, 0x09, 0x01, 0xf7];
  const gs = [0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7];
  const bulk = [...Array.from({ length: 200 }, (_, i) => i & 0x7f), 0xf7];
  const evs = [sysex(0, gmOn), sysex(0, gs), on(10, 0, 60), sysex(20, bulk), on(30, 0, 64), off(96, 0, 60), off(96, 0, 64)];
  const s = parseMidi(encodeMidi(0, 96, [evs]));
  assert.deepEqual(notesOf(s, 0), [[100, 860, 60], [300, 660, 64]]);
  assert.equal(s.ignored.sysex, 0);
  assert.deepEqual(s.parts[0].events?.filter((e) => e.data[0] === 0xf0).map((e) => e.data), [[0xf0, ...gmOn], [0xf0, ...gs], [0xf0, ...bulk]]);
  assert.equal(s.end, 960);
});

test('physical note-offs stay intact; sustain gate lengths are available for synths without pedals', () => {
  const s = parseMidi(
    encodeMidi(0, 100, [[cc(0, 0, 64, 127), ...note(0, 50, 0, 60), ...note(100, 50, 0, 62), on(150, 0, 60), cc(300, 0, 64, 0), off(400, 0, 60)]]),
  );
  // 60: held by the pedal until key 60 is struck again at 150; 62: until the pedal lifts at 300;
  // the second 60 is still held by its key when the pedal lifts, so it ends at its note off
  assert.deepEqual(notesOf(s, 0), [[0, 480, 60], [960, 480, 62], [1440, 2400, 60]]);
  assert.deepEqual(s.parts[0].notes.map((n) => n.sustainLen), [2880, 1920, 2400]);
  assert.deepEqual(s.parts[0].events?.map((e) => e.data), [[0xb0, 64, 127], [0xb0, 64, 0]]);
});

test('notes still sounding at the end of a track end there; very short notes get a minimum', () => {
  const s = parseMidi(encodeMidi(0, 960, [[on(0, 0, 60), on(0, 9, 36), off(0, 9, 36), { t: 1920, d: [0xff, 0x01, 1, 65] }]]));
  assert.deepEqual(notesOf(s, 0), [[0, 1920, 60]]);
  assert.deepEqual(notesOf(s, 1), [[0, MIN_NOTE_TICKS, 36]]);
});

test('volume / expression / pan are timed messages, never baked into note velocity', () => {
  const s = parseMidi(encodeMidi(0, 960, [[cc(0, 0, 7, 127), cc(0, 0, 10, 127), ...note(0, 960, 0, 60, 100), cc(960, 0, 7, 64), ...note(960, 960, 0, 60, 100), cc(1920, 0, 10, 0)]]));
  const p = s.parts[0];
  assert.equal(p.volume, 1);
  assert.equal(p.pan, 0);
  assert.ok(Math.abs(p.notes[0].vel - 100 / 127) < 1e-3);
  assert.equal(p.notes[1].vel, p.notes[0].vel);
  assert.deepEqual(p.events?.map((e) => [e.tick, e.data]), [[0, [176, 7, 127]], [0, [176, 10, 127]], [960, [176, 7, 64]], [1920, [176, 10, 0]]]);
});

test('program changes and the full 14-bit pitch bend are retained at their positions', () => {
  const s = parseMidi(encodeMidi(0, 96, [[prog(0, 0, 24), ...note(0, 96, 0, 50), prog(96, 0, 30), bend(100, 0, 9000), ...note(192, 96, 0, 52)]]));
  assert.equal(s.parts[0].program, 24);
  assert.equal(s.parts[0].programChanges, 1);
  assert.equal(s.ignored.pitchBend, 0);
  assert.deepEqual(s.parts[0].events?.map((e) => [e.tick, e.data]), [[0, [192, 24]], [960, [192, 30]], [1000, [224, 40, 70]]]);
});

test('tempo map: sorted across tracks, default 120 BPM before the first change', () => {
  const s = parseMidi(encodeMidi(1, 480, [[tempo(960, 100), tempo(1920, 140)], [...note(0, 480, 0, 60)]]));
  assert.deepEqual(
    s.tempos.map((x) => [x.tick, Math.round(x.bpm)]),
    [[0, 120], [1920, 100], [3840, 140]],
  );
});

test('unsupported files say why', () => {
  const head = (fmt: number, div: number) => new Uint8Array([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, fmt, 0, 1, div >> 8, div & 255, 0x4d, 0x54, 0x72, 0x6b, 0, 0, 0, 4, 0, 0xff, 0x2f, 0]);
  assert.throws(() => parseMidi(head(2, 96)), /format 2/);
  assert.throws(() => parseMidi(head(1, 0xe728)), /SMPTE/);
  assert.throws(() => parseMidi(new TextEncoder().encode('RIFF....RMIDdata')), /RIFF/);
  assert.throws(() => parseMidi(new TextEncoder().encode('hello world, not midi')), /not a MIDI file/);
  assert.throws(() => parseMidi(head(1, 96).subarray(0, 14)), /no tracks/);
});

test('a truncated file keeps what it has, with a warning', () => {
  const f = encodeMidi(1, 96, [[...note(0, 96, 0, 60), ...note(96, 96, 0, 62), ...note(192, 96, 0, 64)]]);
  const s = parseMidi(f.subarray(0, f.length - 9));
  assert.ok(s.warnings.some((w) => /cut short|truncated/.test(w)));
  assert.ok(s.parts[0].notes.length >= 2);
});

test('random and corrupted bytes never crash the reader (MidiError or a result)', () => {
  let seed = 12345;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) / 16777216;
  const base = encodeMidi(1, 96, [[tempo(0, 120)], [prog(0, 0, 5), ...note(0, 96, 0, 60), cc(10, 0, 64, 127), ...note(96, 300, 0, 64), bend(5, 0, 100)]]);
  for (let i = 0; i < 3000; i++) {
    const f = new Uint8Array(base);
    const flips = 1 + Math.floor(rnd() * 6);
    for (let k = 0; k < flips; k++) f[14 + Math.floor(rnd() * (f.length - 14))] = Math.floor(rnd() * 256);
    try {
      const s = parseMidi(f.subarray(0, 14 + Math.floor(rnd() * (f.length - 13))));
      for (const p of s.parts)
        for (const n of p.notes) assert.ok(n.len >= MIN_NOTE_TICKS && n.start >= 0 && n.key >= 0 && n.key < 128 && n.vel > 0 && n.vel <= 1);
    } catch (e) {
      assert.ok(e instanceof MidiError, String(e));
    }
  }
});


test('every CC, pressure, bank/program and system message survives without dropped bytes', () => {
  const events = Array.from({ length: 128 }, (_, i) => cc(i, 2, i, (127 - i)));
  const other = [
    { t: 200, d: [0xa2, 60, 77] }, { t: 201, d: [0xd2, 93] }, bend(202, 2, 16383), prog(203, 2, 91),
    { t: 204, d: [0xf1, 12] }, { t: 205, d: [0xf2, 1, 2] }, { t: 206, d: [0xf3, 4] },
    { t: 207, d: [0xf6] }, ...[0xf8, 0xfa, 0xfb, 0xfc, 0xfe].map((x, i) => ({ t: 210 + i, d: [x] })),
  ];
  const song = parseMidi(encodeMidi(0, 960, [[...events, ...other, ...note(300, 100, 2, 60)]]));
  assert.deepEqual(song.parts[0].events?.map((e) => [e.tick, e.data]), [...events, ...other].map((e) => [e.t, e.d]));
  assert.deepEqual(song.ignored, { pitchBend: 0, controllers: 0, aftertouch: 0, sysex: 0 });
});

test('channel state and controls in another format-1 track apply before its first note', () => {
  const song = parseMidi(encodeMidi(1, 960, [
    [cc(0, 3, 0, 2), cc(0, 3, 32, 7), prog(0, 3, 40), bend(500, 3, 1)],
    [...note(0, 960, 3, 64)],
  ]));
  assert.equal(song.parts[0].program, 40); assert.equal(song.parts[0].bank, 263);
  assert.equal(song.parts[0].events?.length, 4);
  assert.ok(song.parts[0].events![2].order < song.parts[0].notes[0].onOrder!);
});

test('messages before and after a same-tick note keep their relative order', () => {
  const song = parseMidi(encodeMidi(0, 960, [[prog(0, 0, 1), on(0, 0, 60), prog(0, 0, 2), off(960, 0, 60)]]));
  const part = song.parts[0];
  assert.ok(part.events![0].order < part.notes[0].onOrder!);
  assert.ok(part.notes[0].onOrder! < part.events![1].order);
});

test('sostenuto captures only keys already down, and reset controllers releases pedal-held notes', () => {
  const song = parseMidi(encodeMidi(0, 960, [[on(0, 0, 60), cc(100, 0, 66, 127), on(150, 0, 64), off(200, 0, 60), off(250, 0, 64), cc(500, 0, 121, 0)]]));
  assert.deepEqual(song.parts[0].notes.map((n) => [n.key, n.len, n.sustainLen]), [[60, 200, 500], [64, 100, 100]]);
});

test('unknown metadata and escaped / continued SysEx are preserved', () => {
  const song = parseMidi(encodeMidi(0, 960, [[{ t: 0, d: [0xff, 5, 2, 65, 66] },
    sysex(0, [0x7f, 0x7f, 4, 1]), { t: 10, d: [0xf7, 3, 0, 64, 0xf7] }, ...note(20, 80, 0, 60)]]));
  assert.deepEqual(song.metadata?.map((e) => e.data), [[0xff, 5, 65, 66]]);
  assert.deepEqual(song.parts[0].events?.map((e) => e.data), [[0xf0, 0x7f, 0x7f, 4, 1], [0xf7, 0, 64, 0xf7]]);
});


test('MIDI Port metadata keeps channel state and note-offs on different ports independent', () => {
  const port = (n: number) => ({ t:0, d:[0xff,0x21,1,n] });
  const song = parseMidi(encodeMidi(1,960,[
    [port(0),prog(0,0,0),...note(0,960,0,60),bend(500,0,0)],
    [port(1),prog(0,0,40),...note(0,1920,0,60),cc(500,0,10,127)],
  ]));
  assert.deepEqual(song.parts.map((p)=>[p.port,p.channel,p.program,p.notes[0].len]),[[0,0,0,960],[1,0,40,1920]]);
  assert.equal(song.parts[0].events!.some((e)=>e.data[1]===10 && e.data[0]===176),false);
  assert.equal(song.parts[1].events!.some((e)=>e.data[0]===224),false);
});
