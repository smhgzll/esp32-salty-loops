// SPDX-License-Identifier: GPL-3.0-or-later
// Load estimate and fitting for MIDI imports (load.ts): run with `npm test` in web/.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { estimateLoad, fitLoad, LOAD_LIMIT, sf2MinVoices, sf2Voices, voicesNeeded } from './load.ts';

const n = (start: number, len: number, key = 60) => ({ start, len, key, vel: 0.8 });
const MACRO = 'org.smhgzll.macrosynth', OSC = 'org.smhgzll.3xosc', DRUM = 'org.smhgzll.drumlab', SINE = 'org.smhgzll.sine', SF2 = 'org.smhgzll.soundfont';

test('voices: the most notes sounding at once (touching notes do not overlap)', () => {
  assert.equal(voicesNeeded([]), 1);
  assert.equal(voicesNeeded([n(0, 480), n(480, 480), n(960, 480)]), 1);
  assert.equal(voicesNeeded([n(0, 960, 60), n(0, 960, 64), n(0, 960, 67), n(960, 480)]), 3);
  assert.equal(voicesNeeded([n(0, 1000), n(500, 1000), n(900, 50)]), 3);
});

test('the estimate matches the board measurements of an imported song (within reason)', () => {
  // two MacroSynth guitars + bass + drums: 2 voices each measured 55-61 % (fits), 4 voices skipped notes
  const song = (v: number) => [
    { plugin: MACRO, voices: v },
    { plugin: MACRO, voices: v },
    { plugin: OSC, voices: 1 },
    { plugin: DRUM, voices: 5 },
  ];
  assert.ok(estimateLoad(song(2)) <= LOAD_LIMIT);
  assert.ok(estimateLoad(song(2)) >= 55);
  assert.ok(estimateLoad(song(4)) > LOAD_LIMIT);
});

test('fitting lightens only automatic choices, the costliest first, until the estimate fits', () => {
  const parts = [
    { plugin: MACRO, preset: 'Plucked String', voices: 2, auto: true },
    { plugin: MACRO, preset: 'Plucked String', voices: 2, auto: true },
    { plugin: MACRO, preset: 'Plucked String', voices: 2, auto: false },
    { plugin: MACRO, preset: 'Vowel Pad', voices: 3, auto: true },
    { plugin: DRUM, preset: 'Default Kit', voices: 4, auto: true },
  ];
  const changed = fitLoad(parts, () => true);
  assert.ok(estimateLoad(parts) <= LOAD_LIMIT);
  assert.ok(changed.length >= 1 && !changed.includes(2) && !changed.includes(4));
  assert.equal(parts[2].plugin, MACRO); // the user's own choice stays
  assert.equal(parts[3].plugin, OSC); // the costliest automatic one went first
  assert.equal(parts[3].preset, 'Warm Pad');
  // a song that fits is left alone
  const ok = [{ plugin: MACRO, preset: 'Plucked String', voices: 2, auto: true }, { plugin: OSC, preset: 'Fat Bass', voices: 1, auto: true }];
  assert.deepEqual(fitLoad(ok, () => true), []);
  // automatic effects go when no lighter instrument is left; chosen ones stay
  const withFx = [
    { plugin: DRUM, preset: 'Default Kit', voices: 8, auto: true, fx: ['org.smhgzll.fx.distortion'] },
    { plugin: DRUM, preset: 'Default Kit', voices: 8, auto: false, fx: ['org.smhgzll.fx.distortion'] },
  ];
  assert.ok(estimateLoad(withFx) > LOAD_LIMIT);
  assert.deepEqual(fitLoad(withFx, () => true), [0]);
  assert.deepEqual(withFx[0].fx, []);
  assert.equal(withFx[1].fx.length, 1);
  // no lighter instrument in this firmware: unchanged
  const heavy = Array.from({ length: 6 }, () => ({ plugin: OSC, preset: 'Pluck', voices: 8, auto: true }));
  fitLoad(heavy, (p) => p !== SINE);
  assert.ok(heavy.every((p) => p.plugin === OSC));
});

test('SoundFont parts: sample voices by their notes, fewer (the most first) when the song does not fit', () => {
  // layered presets and releases: a part plays several times as many samples as notes, in limits
  assert.equal(sf2Voices(1, false), 6);
  assert.equal(sf2Voices(4, false), 16);
  assert.equal(sf2Voices(9, false), 24);
  assert.equal(sf2Voices(1, true), 8);
  assert.equal(sf2MinVoices(1), 4);
  assert.equal(sf2MinVoices(5), 10);
  const part = (need: number, drums = false, auto = true) => ({ plugin: SF2, voices: sf2Voices(need, drums), minVoices: sf2MinVoices(need), auto });
  // the nine-part GM song measured on the board fits as it is (45 % at most there)
  const nine = [part(2), part(4), part(2), part(2), part(5), part(2), part(6), part(1, true), part(3, true)];
  assert.ok(estimateLoad(nine) <= LOAD_LIMIT && estimateLoad(nine) >= 45);
  assert.deepEqual(fitLoad(nine, () => true), []);
  // a denser one of sixteen parts (pads, guitars, drum tracks): too many sample voices at first
  const song = [part(2), part(4), part(4), part(2), part(5), part(6), part(6), part(3), part(4), part(2), part(5), part(4), part(3), part(2), part(4, true), part(3, true)];
  const before = song.map((p) => p.voices);
  assert.ok(estimateLoad(song) > LOAD_LIMIT);
  const changed = fitLoad(song, () => true);
  assert.ok(estimateLoad(song) <= LOAD_LIMIT);
  assert.ok(changed.length >= 1);
  for (let k = 0; k < song.length; k++) {
    assert.ok(song[k].voices <= before[k] && song[k].voices >= song[k].minVoices);
    assert.equal(song[k].plugin, SF2); // the instrument stays: only its voices change
  }
  // the parts with the most voices gave the most
  assert.ok(before[5] - song[5].voices >= before[0] - song[0].voices);
  // a song that fits keeps every voice
  const small = [part(2), part(3, true)];
  assert.deepEqual(fitLoad(small, () => true), []);
  assert.deepEqual(small.map((p) => p.voices), [8, 12]);
});
