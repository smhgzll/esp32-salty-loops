// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { canonicalMidiOrder, midiPresets, noteRow, pedalNotes, sliceMidi, systemKind, systemPackets, unmappedMidi } from './expressive.ts';
import { parseMidi } from './smf.ts';
import { cc, encodeMidi, note, prog, sysex } from './testing.ts';
import { arrange, mergeParts, tempoAutomation } from './arrange.ts';
import { SOUNDFONT } from './gm.ts';

test('piano range restores setup in chronological order before its first note', () => {
  const s = parseMidi(encodeMidi(0, 960, [[cc(0, 0, 0, 1), cc(1, 0, 32, 2), prog(2, 0, 40),
    cc(3, 0, 101, 0), cc(4, 0, 100, 0), cc(5, 0, 6, 12), ...note(960, 240, 0, 60), cc(960, 0, 7, 80)]]));
  const a = sliceMidi(s.parts, 960, 1920);
  assert.deepEqual(a.events.slice(0, 6).map((e) => e.data), [[176,0,1],[176,32,2],[192,40],[176,101,0],[176,100,0],[176,6,12]]);
  assert.ok(a.events.every((e) => e.tick === 0));
  assert.ok(a.events.slice(1, 6).every((e, i) => e.order > a.events[i].order));
  const onOrder = s.parts[0].notes[0].onOrder! + a.shift;
  assert.ok(a.events[5].order < onOrder && onOrder < a.events[6].order);
});

test('all future bank/program choices and reset default are prepared selectively', () => {
  const s = parseMidi(encodeMidi(0, 960, [[sysex(0, [0x7e,0x7f,9,1,0xf7]), prog(0,0,40), ...note(0,100,0,60),
    cc(200,0,0,2), cc(200,0,32,3), prog(200,0,41), ...note(240,100,0,64)]]));
  const picks = midiPresets(s.parts[0]);
  for (const expected of [{ bank:0,program:0 },{ bank:0,program:40 },{ bank:259,program:41 },{ bank:0,program:41 }])
    assert.ok(picks.some((p) => p.bank === expected.bank && p.program === expected.program));
  assert.equal(picks.length, 4);
});

test('fixed synths retain pedal gates without changing the original SoundFont note-offs', () => {
  const s = parseMidi(encodeMidi(0, 960, [[cc(0,0,64,127), ...note(0,240,0,60), cc(960,0,64,0)]]));
  assert.equal(pedalNotes(s.parts[0], SOUNDFONT)[0].len, 240);
  assert.equal(pedalNotes(s.parts[0], 'org.smhgzll.sine')[0].len, 960);
  assert.equal(noteRow(pedalNotes(s.parts[0], SOUNDFONT)[0], 1)[7], 64/127);
});

test('explicit global SysEx coverage and split packets do not claim vendor-dump emulation', () => {
  assert.equal(systemKind([0xf0,0x7e,0x7f,9,1,0xf7]), 'reset');
  const gs = [0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7];
  assert.equal(systemKind(gs), 'reset'); const invalid = [...gs]; invalid[9] = 1; assert.equal(systemKind(invalid), null);
  assert.equal(systemKind([0xf0,0x43,0x10,0x4c,0,0,0x7e,0,0xf7]), 'reset');
  assert.equal(systemKind([0xf0,0x41,0x10,0x16,0x12,1,2,3,4,5,0xf7]), null); // MT-32 data is kept, not synthesized
  const s = parseMidi(encodeMidi(0, 960, [[sysex(0,[0x7f,0x7f,4,1]),{t:10,d:[0xf7,3,0,64,0xf7]},...note(20,100,0,60)]]));
  assert.deepEqual(systemPackets(s.parts[0].events!), [[0xf0,0x7f,0x7f,4,1,0,64,0xf7]]);
});

test('independent channel expression prevents a destructive instrument merge', () => {
  const s = parseMidi(encodeMidi(0, 960, [[prog(0,0,0),prog(0,1,0),cc(0,0,10,0),cc(0,1,10,127),...note(0,100,0,60),...note(0,100,1,64)]]));
  assert.equal(mergeParts(s.parts, 'instrument').length, 2);
  const riff = parseMidi(encodeMidi(0, 960, [[...note(0,960,0,60),...note(3840,960,0,60)]]));
  assert.equal(arrange(riff.parts, 3840, 1).patterns.length, 1); // source order is relative to each tick
});

test('unknown controllers and NRPN are reported as retained without a sound mapping', () => {
  const s = parseMidi(encodeMidi(0, 960, [[cc(0,0,2,99),cc(0,0,91,100),cc(0,0,99,3),...note(0,100,0,60)]]));
  assert.deepEqual(unmappedMidi(s.parts).cc, [2,91]); assert.equal(unmappedMidi(s.parts).nrpn, true);
});

test('dense tempo maps split at the point capacity without silently losing changes', () => {
  const tempos = Array.from({ length: 1400 }, (_, i) => ({ tick: i*960, bpm: 100+i%100 }));
  const a = tempoAutomation(tempos, 1400*960);
  assert.ok(a.length > 2); assert.ok(a.every((x) => x.points.length <= 1024));
  for (const t of tempos.slice(1)) {
    const clip = a.find((x) => t.tick >= x.start && t.tick < x.start+x.length)!;
    assert.ok(clip); assert.ok(clip.points.some((p) => p[0] === t.tick-clip.start && Math.abs(p[1]-(t.bpm-20)/280)<1e-9));
  }
  assert.throws(() => tempoAutomation(Array.from({length:1100},(_,i)=>({tick:i,bpm:100+i%100})),2000), /tempo changes/);
});

test('setup and unrelated channel events do not duplicate repeated riffs', () => {
  const s = parseMidi(encodeMidi(0,960,[[prog(0,0,40),cc(0,0,7,100),...note(0,960,0,60),
    ...note(3840,960,0,60),...note(0,240,1,64),...note(3840,240,1,67),...note(3840,240,1,69)]]));
  const p = canonicalMidiOrder(s.parts.find((p) => p.channel === 0)!);
  assert.equal(arrange([p],3840,1).patterns.length,1);
  assert.ok(p.events!.every((e) => e.order < p.notes[0].onOrder!));
});

test('canonical orders keep controls between simultaneous notes and pedal releases', () => {
  const source = { track:0,channel:0,name:'Test',trackName:'',instrument:'',program:0,bank:0,drums:false,
    volume:1,pan:0,programChanges:0,
    notes:[{start:0,len:960,key:60,vel:1,onOrder:10,offOrder:2,sustainLen:1920,sustainOffOrder:6},
      {start:0,len:960,key:64,vel:1,onOrder:30,offOrder:4}],
    events:[{tick:0,order:20,data:[0xb0,7,50]},{tick:1920,order:6,data:[0xb0,64,0]}] };
  const p = canonicalMidiOrder(source);
  assert.ok(p.notes[0].onOrder! < p.events![0].order && p.events![0].order < p.notes[1].onOrder!);
  assert.equal(p.notes[0].sustainOffOrder,p.events![1].order);
});
