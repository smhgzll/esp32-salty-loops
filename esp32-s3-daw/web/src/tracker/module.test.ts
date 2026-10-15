// SPDX-License-Identifier: GPL-3.0-or-later
// Tracker module headers and import plans (module.ts): run with `npm test` in web/.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { channelGroups, fmtTime, importBytes, importPlan, isModuleFile, libraryName, MAX_SPAN, moduleCrc, projectTempo, readModuleHeader, songSpan, splitsFor } from './module.ts';

test('module identity uses the library CRC, including same-size content changes', () => {
  assert.equal(moduleCrc(new Uint8Array()), 0);
  assert.equal(moduleCrc(new TextEncoder().encode('123456789')), 0xcbf43926);
  assert.notEqual(moduleCrc(new Uint8Array([1, 2, 3])), moduleCrc(new Uint8Array([1, 2, 4])));
});

const ascii = (b: Uint8Array, at: number, s: string) => s.split('').forEach((c, i) => (b[at + i] = c.charCodeAt(0)));

function mod(sig: string, title = 'my song') {
  const b = new Uint8Array(1084 + 1024);
  ascii(b, 0, title);
  ascii(b, 1080, sig);
  return b;
}

test('MOD signatures the device accepts, and the others', () => {
  assert.deepEqual(readModuleHeader(mod('M.K.')), { format: 'MOD', title: 'my song', channels: 4 });
  assert.equal(readModuleHeader(mod('6CHN'))?.channels, 6);
  assert.equal(readModuleHeader(mod('16CH'))?.channels, 16);
  for (const sig of ['FLT4', 'M!K!', '0CHN', '00CH', '33CH', '....']) assert.equal(readModuleHeader(mod(sig)), null, sig);
  assert.equal(readModuleHeader(mod('M.K.').slice(0, 1083)), null); // the signature is at 1080
});

test('XM, S3M and IT headers', () => {
  const xm = new Uint8Array(400);
  ascii(xm, 0, 'Extended Module: ');
  ascii(xm, 17, 'Surf');
  xm[68] = 21;
  assert.deepEqual(readModuleHeader(xm), { format: 'XM', title: 'Surf', channels: 21 });
  const s3m = new Uint8Array(200);
  ascii(s3m, 0, 'Beat');
  ascii(s3m, 44, 'SCRM');
  s3m.fill(255, 64, 96);
  s3m.set([0, 8, 1, 9, 2, 10], 64);
  assert.deepEqual(readModuleHeader(s3m), { format: 'S3M', title: 'Beat', channels: 6 });
  const it = new Uint8Array(200);
  ascii(it, 0, 'IMPM');
  ascii(it, 4, 'Pulse');
  it.fill(32 | 128, 64, 128);
  it.fill(32, 64, 72);
  assert.deepEqual(readModuleHeader(it), { format: 'IT', title: 'Pulse', channels: 8 });
  // control characters in a title become spaces
  const x = mod('M.K.', 'a\u0001b');
  assert.equal(readModuleHeader(x)?.title, 'a b');
  assert.equal(readModuleHeader(new Uint8Array(10)), null);
});

test('file names', () => {
  for (const n of ['a.mod', 'B.XM', 'c.s3m', 'd.It']) assert.ok(isModuleFile({ name: n }), n);
  for (const n of ['a.mid', 'a.wav', 'a.sf2', 'mod']) assert.ok(!isModuleFile({ name: n }), n);
});

test('project tempo: the song tempo, halved or doubled into 20..300 BPM', () => {
  assert.equal(projectTempo(125), 125);
  assert.equal(projectTempo(750), 187.5);
  assert.equal(projectTempo(12), 24);
  assert.equal(projectTempo(0), 125);
});

test('the song as one note in one pattern of whole bars', () => {
  // 7.68 s at 125 BPM: 16 beats = 4 bars exactly
  assert.deepEqual(songSpan(7.68, 125), { note: 15360, pattern: 15360 });
  // 10.98 s at 125 BPM: 22.875 beats -> 6 bars
  const s = songSpan(10.98, 125)!;
  assert.equal(s.note, Math.ceil(10.98 * 125 * 16));
  assert.equal(s.pattern, 6 * 3840);
  // a 3/4 bar
  assert.equal(songSpan(10.98, 125, 2880)!.pattern, 8 * 2880);
  // longer than 1000 bars does not fit
  assert.equal(songSpan(3600, 300), null);
  assert.ok(songSpan(1000 * 4 * 60 / 125, 125)!.pattern <= MAX_SPAN);
});

test('channel groups', () => {
  assert.deepEqual(channelGroups(8, 'one', 'Song'), [{ first: 1, last: 64, name: 'Song' }]);
  assert.deepEqual(channelGroups(3, 'each', 'S').map((g) => [g.first, g.last, g.name]), [[1, 1, 'S 1'], [2, 2, 'S 2'], [3, 3, 'S 3']]);
  assert.deepEqual(channelGroups(10, 4, 'S').map((g) => [g.first, g.last]), [[1, 4], [5, 8], [9, 10]]);
  assert.deepEqual(splitsFor(4), ['one', 'each']);
  assert.deepEqual(splitsFor(12), ['one', 8, 4, 'each']);
  assert.deepEqual(splitsFor(40), ['one', 8, 4]);
  assert.equal(importBytes(100000, 300, 4), 100000 + 4 * 300 * 1024);
  assert.equal(fmtTime(59.6), '1:00');
  assert.equal(fmtTime(178.92), '2:59');
});

test('library names as the device stores them', () => {
  assert.equal(libraryName('baris manco - gulpembe.mod'), 'baris manco - gulpembe.mod');
  assert.equal(libraryName('Barış.xm'), 'Bar____.xm'); // two UTF-8 bytes each
  assert.equal(libraryName(' .hidden.it '), 'hidden.it');
  const long = libraryName('a'.repeat(60) + '.s3m');
  assert.equal(long.length, 39);
  assert.ok(long.endsWith('.s3m'));
});

test('an import plan arranges the song at a tempo the DAW has, for the project and the plugin alike', () => {
  const info = { length: 14.08, song_tempo: 750, channels: 1, format: 4, orders: 2, instruments: 1, memory: 253 };
  const p = importPlan(info, 'one', 'Fast');
  assert.equal(p.songTempo, 187.5); // a speed-1 song at 125 BPM: eight rows per beat
  assert.equal(p.span!.note, Math.ceil(14.08 * 187.5 * 16));
  assert.equal(p.groups.length, 1);
  const q = importPlan({ ...info, song_tempo: 125, channels: 8 }, 4, 'S');
  assert.equal(q.songTempo, 125);
  assert.deepEqual(q.groups.map((g) => [g.first, g.last]), [[1, 4], [5, 8]]);
});
