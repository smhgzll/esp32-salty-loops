// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI import plans and honest playback coverage; pure functions, no UI dependencies.
import { SOUNDFONT } from './gm.ts';
import type { MidiMessage, MidiNote, MidiPart } from './smf.ts';

// Engines without native pedals keep the complete note gate offline; no runtime pending-note
// table or new voice ceiling is needed. SoundFont receives the original physical note-offs.
export const pedalNotes = (part: MidiPart, plugin: string) => plugin === SOUNDFONT ? part.notes : part.notes.map((n) => ({
  ...n, len: n.sustainLen ?? n.len, offOrder: n.sustainOffOrder ?? n.offOrder,
}));
export const midiRows = (events: MidiMessage[]) => events.map((e) => [e.tick, e.order, ...e.data]);
export const noteRow = (n: MidiNote, id: number) => [id, n.start, n.len, n.key, n.vel, 0, 0, n.rel ?? .5, 1, 0, n.onOrder ?? 0, n.offOrder ?? 0];
/** Orders only compare messages for the same instrument at the same tick. Anchor notes at
 * stable positions so setup messages and unrelated channels do not prevent riff sharing.
 * Keep room between notes for controls; unusually dense ticks retain their source orders. */
export function canonicalMidiOrder(part: MidiPart): MidiPart {
  const ticks = new Map<number, Map<number, boolean>>();
  const add = (tick: number, order: number | undefined, note: boolean) => {
    if (!order) return;
    const at = ticks.get(tick) ?? new Map<number, boolean>();
    at.set(order, (at.get(order) ?? true) && note); ticks.set(tick, at);
  };
  for (const n of part.notes) {
    add(n.start, n.onOrder, true); add(n.start+n.len, n.offOrder, true);
    if (n.sustainLen !== undefined) add(n.start+n.sustainLen, n.sustainOffOrder, true);
  }
  for (const e of part.events ?? []) add(e.tick, e.order, false);
  const maps = new Map<number, Map<number, number>>();
  for (const [tick, at] of ticks) {
    const entries = [...at].sort((a,b) => a[0]-b[0]);
    const map = new Map<number, number>(); let anchor = 0, run = 0, valid = true;
    for (const [order, note] of entries) {
      if (note) { anchor = anchor ? anchor+4096 : 1048576; run = 0; map.set(order, anchor); }
      else { ++run; if (anchor && run >= 4096) valid = false; map.set(order, anchor+run); }
      if (anchor+run > 0x7fffffff) valid = false;
    }
    maps.set(tick, valid ? map : new Map(entries.map(([order]) => [order, order])));
  }
  const orderAt = (tick: number, order: number | undefined) => order ? maps.get(tick)?.get(order) ?? order : order;
  return { ...part,
    notes: part.notes.map((n) => ({ ...n, onOrder: orderAt(n.start,n.onOrder), offOrder: orderAt(n.start+n.len,n.offOrder),
      sustainOffOrder: orderAt(n.start+(n.sustainLen ?? n.len),n.sustainOffOrder) })),
    events: part.events?.map((e) => ({ ...e, order: orderAt(e.tick,e.order)! })),
  };
}
/** All patches a stream can choose. Prepare them on the control thread, never at a note-on. */
export function midiPresets(part: MidiPart) {
  let msb = part.bank >> 7, lsb = part.bank & 127;
  const result = new Map<string, { bank: number; program: number }>();
  const add = (bank: number, program: number) => result.set(`${bank}:${program}`, { bank, program });
  add(part.drums ? 128 : part.bank, Math.max(0, part.program));
  if (systemPackets(part.events ?? []).some((p) => systemKind(p) === 'reset')) add(part.drums ? 128 : 0, 0);
  for (const e of part.events ?? []) {
    const hi = e.data[0] & 0xf0;
    if (hi === 0xb0 && e.data[1] === 0) msb = e.data[2];
    else if (hi === 0xb0 && e.data[1] === 32) lsb = e.data[2];
    else if (hi === 0xc0) { add(part.drums ? 128 : msb * 128 + lsb, e.data[1]); add(part.drums ? 128 : 0, e.data[1]); }
  }
  return [...result.values()];
}

/** Reassemble SMF F0/F7 fragments without losing their stored original messages. */
export function systemPackets(events: MidiMessage[]) {
  const packets: number[][] = []; let pending: number[] = [];
  for (const e of events) {
    const [status, ...body] = e.data;
    if (status === 0xf0) pending = [status];
    if (status === 0xf0 || status === 0xf7) {
      if (!pending.length) { packets.push(e.data); continue; }
      for (const b of body) {
        if (b >= 0xf8) { packets.push([b]); continue; }
        pending.push(b);
        if (b === 0xf7) { packets.push(pending); pending = []; }
      }
    } else if (status >= 0xf0 && status !== 0xff) packets.push(e.data);
  }
  if (pending.length) packets.push(pending);
  return packets;
}
/** The exact documented global messages implemented by the SDK (not vendor patch emulation). */
export function systemKind(d: number[]): 'reset' | 'volume' | 'balance' | 'tuning' | null {
  if (d[0] !== 0xf0 || d[d.length - 1] !== 0xf7 || d.slice(1, -1).some((x) => x > 127)) return null;
  if (d.length === 6 && d[1] === 0x7e && d[3] === 9 && [1, 3].includes(d[4])) return 'reset';
  if (d.length === 8 && d[1] === 0x7f && d[3] === 4) {
    if (d[4] === 1) return 'volume'; if (d[4] === 2) return 'balance';
    if (d[4] === 3 || d[4] === 4) return 'tuning';
  }
  if (d.length === 11 && d[1] === 0x41 && d[3] === 0x42 && d[4] === 0x12 && d[5] === 0x40 && d[6] === 0 &&
      (d.slice(5, 10).reduce((a, b) => a + b, 0) & 127) === 0) {
    if (d[7] === 0x7f && d[8] === 0) return 'reset';
    if (d[7] === 4) return 'volume'; if (d[7] === 5) return 'tuning';
  }
  if (d.length === 9 && d[1] === 0x43 && (d[2] & 0xf0) === 0x10 && d[3] === 0x4c && d[4] === 0 && d[5] === 0) {
    if ([0x7e, 0x7f].includes(d[6]) && d[7] === 0) return 'reset';
    if (d[6] === 4) return 'volume'; if (d[6] === 6) return 'tuning';
  }
  return null;
}
const mappedCC = new Set([0,1,6,7,8,10,11,32,38,39,40,42,43,64,66,67,96,97,98,99,100,101,120,121,123]);
const genericCC = new Set([6,7,8,10,11,38,39,40,42,43,64,66,67,96,97,98,99,100,101,120,121,123]);
export function unmappedMidi(parts: MidiPart[], plugins?: string[]) {
  const cc = new Set<number>(); let nrpn = false;
  parts.forEach((part, k) => {
    const mapped = !plugins || plugins[k] === SOUNDFONT ? mappedCC : genericCC;
    for (const e of part.events ?? []) if ((e.data[0] & 0xf0) === 0xb0) {
      const n = e.data[1]; if (!mapped.has(n)) cc.add(n); if (n === 98 || n === 99) nrpn = true;
    }
  });
  const unique = [...new Map(parts.flatMap((p) => p.events ?? []).map((e) => [JSON.stringify([e.tick,e.order,e.data]), e])).values()]
    .sort((a,b) => a.tick-b.tick || a.order-b.order);
  const unknown = systemPackets(unique).filter((p) => !systemKind(p) && p[0] === 0xf0).length;
  return { cc: [...cc].sort((a,b) => a-b), nrpn, unknown };
}

/** Slice MIDI including the preceding controller setup. Prefix messages need a fresh ordered
 *  range: retaining their per-tick ordinals at the new tick 0 would reorder bank/RPN sequences. */
export function sliceMidi(parts: MidiPart[], from: number, to: number) {
  const unique = [...new Map(parts.flatMap((p) => p.events ?? []).map((e) => [JSON.stringify([e.tick,e.order,e.data]), e])).values()]
    .filter((e) => e.tick < to).sort((a,b) => a.tick-b.tick || a.order-b.order);
  const prefix = unique.filter((e) => e.tick < from);
  const shift = prefix.length * 2;
  const events = unique.map((e, i) => e.tick < from ? { ...e, tick: 0, order: (i + 1) * 2 } : { ...e, tick: e.tick-from, order: e.order+shift });
  return { events, shift };
}
