// SPDX-License-Identifier: GPL-3.0-or-later
// Tracker modules (MOD, XM, S3M, IT) in the browser: what a file is, before it is uploaded, and the
// plan of an import. The device's Tracker plugin (libxmp) plays the song; it reports the exact
// length, channel count and tempo through samples.probe (docs/TRACKER.md).

export const TRACKER = 'org.smhgzll.tracker';
export const MODULE_ACCEPT = '.mod,.xm,.s3m,.it';
export const isModuleFile = (f: { name: string }) => /\.(mod|xm|s3m|it)$/i.test(f.name);

const crcTable = Uint32Array.from({ length: 256 }, (_, i) => {
  let c = i;
  for (let b = 0; b < 8; ++b) c = (c >>> 1) ^ (c & 1 ? 0xedb88320 : 0);
  return c >>> 0;
});
/** Same CRC as the library header: size alone cannot identify a changed file. */
export function moduleCrc(bytes: Uint8Array): number {
  let c = 0xffffffff;
  for (const b of bytes) c = (c >>> 8) ^ crcTable[(c ^ b) & 255];
  return (c ^ 0xffffffff) >>> 0;
}

export type ModuleFormat = 'MOD' | 'XM' | 'S3M' | 'IT';
export const FORMAT_NAMES: Record<ModuleFormat, string> = {
  MOD: 'ProTracker MOD',
  XM: 'FastTracker 2 XM',
  S3M: 'Scream Tracker 3 S3M',
  IT: 'Impulse Tracker IT',
};
/** the Tracker plugin's read-only "format" values */
export const FORMAT_BY_INDEX: (ModuleFormat | null)[] = [null, 'MOD', 'XM', 'S3M', 'IT', null];

export interface ModuleHeader {
  format: ModuleFormat;
  title: string;
  /** the header's channel count: exact for MOD / XM, the enabled channels for S3M / IT */
  channels: number;
}

const text = (b: Uint8Array, at: number, n: number) => {
  let s = '';
  for (let i = at; i < at + n && i < b.length && b[i]; i++) s += b[i] >= 32 && b[i] < 127 ? String.fromCharCode(b[i]) : ' ';
  return s.trim();
};
const tag = (b: Uint8Array, at: number, t: string) => b.length >= at + t.length && t.split('').every((c, i) => b[at + i] === c.charCodeAt(0));
const digit = (c: number) => c >= 48 && c <= 57;

/** What a module is, from its first bytes (the same signatures the device's library accepts:
 * the formats libxmp-lite reads). null: not a module the Tracker plugin plays. */
export function readModuleHeader(b: Uint8Array): ModuleHeader | null {
  if (tag(b, 0, 'IMPM')) {
    let channels = 0;
    for (let c = 0; c < 64 && 64 + c < b.length; c++) if (!(b[64 + c] & 128)) channels++;
    return { format: 'IT', title: text(b, 4, 26), channels };
  }
  if (tag(b, 0, 'Extended Module: ')) return { format: 'XM', title: text(b, 17, 20), channels: b.length >= 70 ? b[68] | (b[69] << 8) : 0 };
  if (tag(b, 44, 'SCRM')) {
    let channels = 0;
    for (let c = 0; c < 32 && 64 + c < b.length; c++) if (b[64 + c] < 16) channels++;
    return { format: 'S3M', title: text(b, 0, 28), channels };
  }
  if (b.length >= 1084) {
    const m = 1080;
    if (tag(b, m, 'M.K.')) return { format: 'MOD', title: text(b, 0, 20), channels: 4 };
    if (digit(b[m]) && b[m] !== 48 && tag(b, m + 1, 'CHN')) return { format: 'MOD', title: text(b, 0, 20), channels: b[m] - 48 };
    if (digit(b[m]) && digit(b[m + 1]) && tag(b, m + 2, 'CH')) {
      const n = (b[m] - 48) * 10 + (b[m + 1] - 48);
      if (n > 0 && n <= 32) return { format: 'MOD', title: text(b, 0, 20), channels: n };
    }
  }
  return null;
}

/** what the device's Tracker plugin reports for a module (samples.probe) */
export interface ModuleInfo {
  length: number; // seconds, the song's first pass at its own speed
  song_tempo: number; // the project tempo at which a quarter note is four rows at the song's first speed
  channels: number;
  format: number; // FORMAT_BY_INDEX
  orders: number;
  instruments: number;
  memory: number; // KB the plugin's player took for this module (one channel)
}

export const PPQ = 960;
export const MIN_BPM = 20;
export const MAX_BPM = 300;
/** the longest pattern / note / clip the device accepts: 1000 bars of 4/4 */
export const MAX_SPAN = PPQ * 4 * 1000;

/** The tempo the song is arranged at: its own, halved or doubled into the DAW's range. Both the
 * project tempo and the Tracker plugin's Song tempo get it, so the song plays at its own speed. */
export function projectTempo(songTempo: number) {
  let t = songTempo > 0 ? songTempo : 125;
  while (t > MAX_BPM) t /= 2;
  while (t < MIN_BPM) t *= 2;
  return Math.round(t * 100) / 100;
}

/** The song as one note: its length in ticks, and the pattern (whole bars) that holds it. The
 * plugin's Song tempo parameter is the song's tempo, so the song takes the same number of beats at
 * any project tempo (it plays faster or slower with the project). null: longer than 1000 bars. */
export function songSpan(lengthSec: number, songTempo: number, barTicks = PPQ * 4) {
  const note = Math.max(1, Math.ceil((lengthSec * songTempo * PPQ) / 60));
  const pattern = Math.ceil(note / barTicks) * barTicks;
  return pattern > MAX_SPAN ? null : { note, pattern };
}

export interface ChannelGroup {
  first: number; // 1-based tracker channels, inclusive
  last: number;
  name: string;
}
export type Split = 'one' | 'each' | 4 | 8;

/** how the song's channels go onto DAW channels: all on one, one each, or in groups */
export function channelGroups(channels: number, split: Split, title: string): ChannelGroup[] {
  const n = Math.max(1, Math.min(64, channels));
  if (split === 'one' || n === 1) return [{ first: 1, last: 64, name: title }];
  const size = split === 'each' ? 1 : split;
  const out: ChannelGroup[] = [];
  for (let a = 1; a <= n; a += size) {
    const b = Math.min(n, a + size - 1);
    out.push({ first: a, last: b, name: a === b ? `${title} ${a}` : `${title} ${a}-${b}` });
  }
  return out;
}

/** the splits worth offering for a song of `channels` channels */
export function splitsFor(channels: number): Split[] {
  const s: Split[] = ['one'];
  if (channels > 8) s.push(8);
  if (channels > 4) s.push(4);
  if (channels > 1 && channels <= 32) s.push('each');
  return s;
}

/** What an import makes of a song: the tempo it is arranged at (project tempo and the plugin's
 * Song tempo), its note and pattern lengths (null: too long), and its channels. */
export function importPlan(info: ModuleInfo, split: Split, title: string, barTicks = PPQ * 4) {
  const songTempo = projectTempo(info.song_tempo);
  return { songTempo, span: songSpan(info.length, songTempo, barTicks), groups: channelGroups(info.channels, split, title) };
}

/** PSRAM an import needs: the file once (shared) and one player per channel */
export const importBytes = (fileBytes: number, playerKb: number, groups: number) => fileBytes + groups * playerKb * 1024;

/** The name the device's sample library stores a file under (sanitizeSampleName in
 * sample_store.cpp): bytes of UTF-8, anything but letters, digits and ` ._-()+` becomes `_`, no
 * leading blanks / dots or trailing blanks, at most 39 bytes keeping a short extension. */
export function libraryName(name: string) {
  const ok = (c: number) => (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57) || ' ._-()+'.includes(String.fromCharCode(c));
  let out = Array.from(new TextEncoder().encode(name), (c) => (ok(c) ? String.fromCharCode(c) : '_')).join('');
  out = out.replace(/^[ .]+/, '').replace(/ +$/, '');
  if (out.length > 39) {
    const dot = out.lastIndexOf('.');
    const ext = dot >= 0 && out.length - dot <= 6 ? out.slice(dot) : '';
    out = out.slice(0, 39 - ext.length) + ext;
  }
  return out;
}

export const fmtTime = (s: number) => {
  const t = Math.round(s);
  return `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`;
};
