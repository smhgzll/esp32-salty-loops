// SPDX-License-Identifier: GPL-3.0-or-later
// Standard MIDI File 0/1 reader. Editable notes retain original velocity, physical note-off,
// release velocity and message order. Every other channel/system/SysEx/meta message is retained
// verbatim with its tick; instruments interpret their supported subset at playback.
// Musical-time SMF files up to 8 MB; RIFF, SMPTE and format 2 remain explicit format errors.
import { GM_PROGRAMS } from './gm.ts';

export const PPQ = 960;
export const MAX_FILE_BYTES = 8 << 20;
export const MAX_NOTES = 200000;
/** shortest note kept (a 1/128 note): drum tracks often switch a note off in the tick it starts */
export const MIN_NOTE_TICKS = 30;

export interface MidiNote {
  start: number; // ticks (960 per quarter note)
  len: number;
  key: number;
  vel: number; // 0..1
  rel?: number;
  onOrder?: number;
  offOrder?: number;
  sustainLen?: number;
  sustainOffOrder?: number;
}

export interface MidiPart {
  track: number; // index of the file's track
  channel: number; // MIDI channel 0..15
  port?: number; // SMF MIDI Port meta event, default 0
  name: string;
  trackName: string;
  instrument: string; // the track's instrument name meta event ('' = none)
  program: number; // GM program 0..127, -1 = none before the first note
  bank: number; // bank select in effect at that program change (MSB * 128 + LSB)
  drums: boolean; // MIDI channel 10
  notes: MidiNote[]; // sorted by start, then key
  volume: number; // the part's loudest CC 7 x CC 11 level, relative to the GM default (CC 7 = 100)
  pan: number; // -1..1, CC 10 before the first note
  programChanges: number;
  events?: MidiMessage[]; // all non-note channel messages, SysEx/system and retained metadata
}

export interface MidiMessage {
  tick: number;
  order: number; // stable original order, including notes
  port?: number; // source port (kept separate when assigning DAW channels)
  data: number[]; // complete status and data bytes (FF + meta type + payload for metadata)
}

export interface MidiSong {
  format: number;
  division: number; // the file's ticks per quarter note
  trackCount: number;
  title: string;
  tempos: { tick: number; bpm: number }[]; // sorted, the first at tick 0
  timeSigs: { tick: number; num: number; den: number }[]; // sorted (may be empty)
  markers: { tick: number; name: string }[];
  parts: MidiPart[];
  end: number; // end of the last note
  ignored: { pitchBend: number; controllers: number; aftertouch: number; sysex: number };
  warnings: string[];
  metadata?: MidiMessage[];
}

export class MidiError extends Error {}

const ascii4 = (d: Uint8Array, o: number) => String.fromCharCode(d[o], d[o + 1], d[o + 2], d[o + 3]);
const u32 = (d: Uint8Array, o: number) => ((d[o] << 24) >>> 0) + (d[o + 1] << 16) + (d[o + 2] << 8) + d[o + 3];
const u16 = (d: Uint8Array, o: number) => (d[o] << 8) | d[o + 1];

/** meta event text: UTF-8 when valid, else Latin-1 (older files), control characters removed */
function text(b: Uint8Array) {
  let s: string;
  try {
    s = new TextDecoder('utf-8', { fatal: true }).decode(b);
  } catch {
    s = String.fromCharCode(...b.subarray(0, 256));
  }
  return s.replace(/[\u0000-\u001f\u007f]/g, ' ').trim().slice(0, 64);
}

export function parseMidi(data: Uint8Array): MidiSong {
  if (data.length > MAX_FILE_BYTES) throw new MidiError(`the file is too large (${(data.length / 1048576).toFixed(1)} MB; at most 8 MB)`);
  if (data.length >= 4 && ascii4(data, 0) === 'RIFF')
    throw new MidiError('RIFF MIDI (.rmi) is not supported: save it as a standard MIDI file (.mid)');
  if (data.length < 14 || ascii4(data, 0) !== 'MThd') throw new MidiError('not a MIDI file (no MThd header)');
  const headLen = u32(data, 4);
  if (headLen < 6) throw new MidiError('broken MIDI header');
  const format = u16(data, 8);
  const trackCount = u16(data, 10);
  const division = u16(data, 12);
  if (format === 2) throw new MidiError('MIDI format 2 (independent sequences) is not supported; formats 0 and 1 are');
  if (format > 2) throw new MidiError(`unknown MIDI format ${format}`);
  if (division & 0x8000) throw new MidiError('SMPTE time code is not supported, only musical time (ticks per quarter note)');
  if (division === 0) throw new MidiError('broken MIDI header (0 ticks per quarter note)');

  const warnings: string[] = [];
  const tracks: Uint8Array[] = [];
  let pos = 8 + headLen;
  while (pos + 8 <= data.length) {
    const id = ascii4(data, pos);
    const len = u32(data, pos + 4);
    pos += 8;
    let end = pos + len;
    if (end > data.length) {
      warnings.push(`track ${tracks.length + 1} is cut short (the file is truncated)`);
      end = data.length;
    }
    if (id === 'MTrk') tracks.push(data.subarray(pos, end));
    pos = end;
  }
  if (!tracks.length) throw new MidiError('the file has no tracks');
  if (tracks.length !== trackCount) warnings.push(`the header announces ${trackCount} tracks, the file has ${tracks.length}`);

  const scale = PPQ / division;
  const tick = (t: number) => Math.round(t * scale);
  const tempos: { tick: number; bpm: number }[] = [];
  const timeSigs: { tick: number; num: number; den: number }[] = [];
  const markers: { tick: number; name: string }[] = [];
  const ignored = { pitchBend: 0, controllers: 0, aftertouch: 0, sysex: 0 };
  type Raw = { tick: number; track: number; index: number; port: number; data: number[] };
  const raw: Raw[] = [];
  const names: string[] = [], instruments: string[] = [];
  let title = '', endTick = 0;
  const MAX_EVENTS = 500000;
  tracks.forEach((t, ti) => {
    let p = 0, abs = 0, status = 0, index = 0, port = 0;
    const vlq = () => {
      let v = 0;
      for (let i = 0; i < 4; ++i) {
        if (p >= t.length) throw new RangeError();
        const b = t[p++]; v = v * 128 + (b & 127);
        if (!(b & 128)) return v;
      }
      throw new MidiError(`track ${ti + 1}: broken variable-length number`);
    };
    const read = () => {
      if (p >= t.length) throw new RangeError();
      const b = t[p++];
      if (b > 127) throw new MidiError(`track ${ti + 1}: status byte in MIDI data`);
      return b;
    };
    const body = (n: number) => {
      if (p + n > t.length) throw new RangeError();
      const b = Array.from(t.subarray(p, p + n)); p += n; return b;
    };
    try {
      while (p < t.length) {
        abs += vlq(); endTick = Math.max(endTick, abs);
        if (p >= t.length) throw new RangeError();
        let st = t[p];
        if (st & 128) ++p;
        else if (status) st = status;
        else throw new MidiError(`track ${ti + 1}: a data byte without a status byte (corrupt file)`);
        let data: number[];
        if (st === 0xff) {
          if (p >= t.length) throw new RangeError();
          const type = t[p++], len = vlq(), bytes = body(len);
          data = [st, type, ...bytes];
          if (type === 0x51 && bytes.length === 3) {
            const us = (bytes[0] << 16) | (bytes[1] << 8) | bytes[2];
            if (us) tempos.push({ tick: tick(abs), bpm: Math.round(60000000000 / us) / 1000 });
          } else if (type === 0x58 && bytes.length >= 2 && bytes[0] >= 1 && bytes[1] <= 6)
            timeSigs.push({ tick: tick(abs), num: bytes[0], den: 1 << bytes[1] });
          else if (type === 3 && !names[ti]) names[ti] = text(new Uint8Array(bytes));
          else if (type === 4 && !instruments[ti]) instruments[ti] = text(new Uint8Array(bytes));
          else if (type === 6) {
            const name = text(new Uint8Array(bytes)); if (name) markers.push({ tick: tick(abs), name });
          }
          if (type === 0x21 && bytes.length === 1 && bytes[0] < 128) port = bytes[0];
          if (type === 0x2f) break;
        } else if (st === 0xf0 || st === 0xf7) {
          const len = vlq(); data = [st, ...body(len)];
        } else if (st >= 0xf0) {
          const n = st === 0xf2 ? 2 : st === 0xf1 || st === 0xf3 ? 1 : 0;
          data = [st]; for (let i = 0; i < n; ++i) data.push(read());
        } else {
          status = st;
          data = [st, read()];
          if ((st & 0xf0) !== 0xc0 && (st & 0xf0) !== 0xd0) data.push(read());
        }
        raw.push({ tick: tick(abs), track: ti, index: index++, port, data });
        if (raw.length > MAX_EVENTS) throw new MidiError(`more than ${MAX_EVENTS} MIDI events`);
      }
    } catch (e) {
      if (e instanceof MidiError) throw e;
      warnings.push(`track ${ti + 1} ends in the middle of an event (truncated)`);
    }
  });
  // Format 1 has one state per MIDI channel, even when its program / controls are in a
  // different track from its notes. File order within a track is stable at equal ticks.
  raw.sort((a, b) => a.tick - b.tick || a.track - b.track || a.index - b.index);
  type State = { msb: number; lsb: number; program: number; bank: number; held: Map<number, MidiNote>; sounding: Map<MidiNote, { released: boolean; sostenuto: boolean }>; pedal: boolean; sostenuto: boolean };
  const states = new Map<string, State>();
  const stateFor = (key: string) => {
    let state = states.get(key);
    if (!state) { state = { msb: 0, lsb: 0, program: -1, bank: 0, held: new Map(), sounding: new Map(), pedal: false, sostenuto: false }; states.set(key, state); }
    return state;
  };
  const parts: MidiPart[] = [], byPart = new Map<string, MidiPart>();
  const controls = new Map<string, MidiMessage[]>();
  const global: MidiMessage[] = [], metadata: MidiMessage[] = [];
  let totalNotes = 0;
  const finish = (n: MidiNote, at: number, order: number, rel = .5) => {
    n.len = Math.max(MIN_NOTE_TICKS, at - n.start); n.offOrder = order; n.rel = rel;
  };
  const releaseHeld = (state: State, at: number, order: number) => {
    for (const [n, h] of state.sounding) if (h.released && !state.pedal && !h.sostenuto) {
      n.sustainLen = Math.max(n.len, at - n.start); n.sustainOffOrder = order; state.sounding.delete(n);
    }
  };
  const keyOff = (state: State, key: number, at: number, order: number, rel = .5) => {
    const n = state.held.get(key); if (!n) return;
    finish(n, at, order, rel); state.held.delete(key);
    const h = state.sounding.get(n); if (h) h.released = true;
    releaseHeld(state, at, order);
  };
  let orderTick = -1, tickOrder = 0;
  raw.forEach((e) => {
    const st = e.data[0], hi = st & 0xf0, c = st & 15, source = `${e.port}|${c}`;
    // Reserve order 0 for DAW edits, and space before a retrigger for a synthetic note-off.
    if (e.tick !== orderTick) { orderTick = e.tick; tickOrder = 0; }
    const order = ++tickOrder * 2;
    const msg = { tick: e.tick, order, data: e.data, port: e.port };
    if (st >= 0xf0) {
      (st === 0xff && e.data.length > 1 ? metadata : global).push(msg); return;
    }
    const state = stateFor(source);
    const d1 = e.data[1], d2 = e.data[2] ?? 0;
    if (hi === 0x90 && d2) {
      const earlier = state.held.get(d1);
      if (earlier) keyOff(state, d1, e.tick, order - 1);
      const key = `${e.track}|${e.port}|${c}`;
      let part = byPart.get(key);
      if (!part) {
        const drums = c === 9;
        part = { track: e.track, channel: c, port: e.port, name: '', trackName: names[e.track] ?? '', instrument: instruments[e.track] ?? '',
          program: state.program, bank: state.bank, drums, notes: [], volume: 1, pan: 0, programChanges: 0 };
        byPart.set(key, part); parts.push(part);
      }
      const n: MidiNote = { start: e.tick, len: MIN_NOTE_TICKS, key: d1, vel: d2 / 127, onOrder: order };
      part.notes.push(n); state.held.set(d1, n); state.sounding.set(n, { released: false, sostenuto: false });
      if (++totalNotes > MAX_NOTES) throw new MidiError(`more than ${MAX_NOTES} notes`);
    } else if (hi === 0x80 || hi === 0x90) {
      keyOff(state, d1, e.tick, order, hi === 0x80 ? d2 / 127 : .5);
    } else {
      const messages = controls.get(source) ?? []; messages.push(msg); controls.set(source, messages);
      if (hi === 0xb0) {
        if (d1 === 0) state.msb = d2;
        else if (d1 === 32) state.lsb = d2;
        else if (d1 === 64) { state.pedal = d2 >= 64; releaseHeld(state, e.tick, order); }
        else if (d1 === 66) {
          const down = d2 >= 64;
          if (down && !state.sostenuto) for (const h of state.sounding.values()) if (!h.released) h.sostenuto = true;
          if (!down) for (const h of state.sounding.values()) h.sostenuto = false;
          state.sostenuto = down; releaseHeld(state, e.tick, order);
        } else if (d1 === 121) {
          state.pedal = state.sostenuto = false;
          for (const h of state.sounding.values()) h.sostenuto = false;
          releaseHeld(state, e.tick, order);
        } else if (d1 === 120 || d1 >= 123) {
          for (const key of [...state.held.keys()]) keyOff(state, key, e.tick, order);
          if (d1 === 120) {
            for (const n of state.sounding.keys()) { n.sustainLen = Math.max(n.len, e.tick - n.start); n.sustainOffOrder = order; }
            state.sounding.clear();
          }
        }
      } else if (hi === 0xc0) {
        state.program = d1; state.bank = state.msb * 128 + state.lsb;
        for (const p of parts) if (p.channel === c && p.port === e.port && p.notes.length) p.programChanges++;
      }
    }
  });
  const last = tick(endTick);
  for (const state of states.values()) {
    for (const key of [...state.held.keys()]) keyOff(state, key, last, 0x7ffffffe);
    state.pedal = false;
    for (const h of state.sounding.values()) h.sostenuto = false;
    releaseHeld(state, last, 0x7ffffffe);
  }
  parts.sort((a, b) => a.track - b.track || (a.port ?? 0) - (b.port ?? 0) || a.channel - b.channel);
  parts.forEach((part) => {
    const peers = parts.filter((p) => p.track === part.track).length;
    let name = format === 0 ? '' : part.trackName.replace(/[\s:]+$/, '');
    if (name && peers > 1) name += ` ${part.port ? `${part.port + 1}.` : ''}${part.channel + 1}`;
    part.name = name || part.instrument || (part.drums ? 'Drums' : part.program >= 0 ? GM_PROGRAMS[part.program] : `Channel ${part.channel + 1}`);
    part.notes.sort((a, b) => a.start - b.start || a.key - b.key);
    part.events = [...(controls.get(`${part.port ?? 0}|${part.channel}`) ?? []), ...global.filter((e) => e.port === part.port)].sort((a, b) => a.tick - b.tick || a.order - b.order);
  });
  if (format === 0) title = names[0] ?? '';
  else {
    const conductor = names.find((n, i) => n && !parts.some((p) => p.track === i)); title = conductor ?? '';
  }

  // tempo map and time signatures: sorted, one entry per tick (the later one wins)
  const byTick = <T extends { tick: number }>(xs: T[]) => {
    const out: T[] = [];
    for (const x of xs.map((v, i) => [v, i] as const).sort((a, b) => a[0].tick - b[0].tick || a[1] - b[1]).map((v) => v[0])) {
      if (out.length && out[out.length - 1].tick === x.tick) out[out.length - 1] = x;
      else out.push(x);
    }
    return out;
  };
  let tm = byTick(tempos);
  if (!tm.length || tm[0].tick > 0) tm.unshift({ tick: 0, bpm: 120 });
  tm = tm.filter((x, i) => i === 0 || Math.abs(x.bpm - tm[i - 1].bpm) > 1e-3);
  const ts = byTick(timeSigs);
  markers.sort((a, b) => a.tick - b.tick);

  let end = 0;
  for (const pt of parts) {
    for (const n of pt.notes) end = Math.max(end, n.start + n.len);
    for (const e of pt.events ?? []) end = Math.max(end, e.tick + 1);
  }
  return { format, division, trackCount: tracks.length, title, tempos: tm, timeSigs: ts, markers, parts, end, ignored, warnings, metadata };
}
