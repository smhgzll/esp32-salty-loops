// SPDX-License-Identifier: GPL-3.0-or-later
// A small Standard MIDI File writer for the tests: tracks of events at absolute ticks.

export interface Ev {
  t: number; // absolute tick in the file's division
  d: number[]; // status and data bytes, or 0xff type ...body for a meta event
}

const vlq = (v: number) => {
  const out = [v & 0x7f];
  while ((v = Math.floor(v / 128)) > 0) out.unshift((v & 0x7f) | 0x80);
  return out;
};
const be32 = (v: number) => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255];

export const on = (t: number, ch: number, key: number, vel = 100): Ev => ({ t, d: [0x90 | ch, key, vel] });
export const off = (t: number, ch: number, key: number, zeroVelOn = false): Ev => ({ t, d: zeroVelOn ? [0x90 | ch, key, 0] : [0x80 | ch, key, 64] });
export const cc = (t: number, ch: number, n: number, v: number): Ev => ({ t, d: [0xb0 | ch, n, v] });
export const prog = (t: number, ch: number, p: number): Ev => ({ t, d: [0xc0 | ch, p] });
export const bend = (t: number, ch: number, v: number): Ev => ({ t, d: [0xe0 | ch, v & 127, v >> 7] });
const meta = (t: number, type: number, body: number[]): Ev => ({ t, d: [0xff, type, ...vlq(body.length), ...body] });
const bytes = (s: string) => [...new TextEncoder().encode(s)];
export const tempo = (t: number, bpm: number) => {
  const us = Math.round(60000000 / bpm);
  return meta(t, 0x51, [(us >> 16) & 255, (us >> 8) & 255, us & 255]);
};
export const timeSig = (t: number, num: number, den: number) => meta(t, 0x58, [num, Math.log2(den), 24, 8]);
export const trackName = (t: number, s: string) => meta(t, 0x03, bytes(s));
export const marker = (t: number, s: string) => meta(t, 0x06, bytes(s));
/** a SysEx event (F0, length, data ending in F7), e.g. the GM System On message */
export const sysex = (t: number, body: number[]): Ev => ({ t, d: [0xf0, ...vlq(body.length), ...body] });

/** a note: its on and off events */
export const note = (t: number, len: number, ch: number, key: number, vel = 100): Ev[] => [on(t, ch, key, vel), off(t + len, ch, key)];

export function encodeTrack(evs: Ev[], runningStatus = true, endOfTrack = true): number[] {
  const sorted = evs.map((e, i) => [e, i] as const).sort((a, b) => a[0].t - b[0].t || a[1] - b[1]).map((x) => x[0]);
  const out: number[] = [];
  let last = 0;
  let status = 0;
  for (const e of sorted) {
    out.push(...vlq(e.t - last));
    last = e.t;
    const s = e.d[0];
    if (runningStatus && s < 0xf0 && s === status) out.push(...e.d.slice(1));
    else out.push(...e.d);
    status = s < 0xf0 ? s : status;
  }
  if (endOfTrack) out.push(0, 0xff, 0x2f, 0);
  return out;
}

export function encodeMidi(format: number, division: number, tracks: Ev[][], runningStatus = true): Uint8Array {
  const out = [...bytes('MThd'), ...be32(6), 0, format, (tracks.length >> 8) & 255, tracks.length & 255, (division >> 8) & 255, division & 255];
  for (const t of tracks) {
    const body = encodeTrack(t, runningStatus);
    out.push(...bytes('MTrk'), ...be32(body.length), ...body);
  }
  return new Uint8Array(out);
}
