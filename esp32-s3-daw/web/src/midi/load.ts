// SPDX-License-Identifier: GPL-3.0-or-later
// How much of the board's audio deadline an imported song is likely to need, from per-plugin costs
// measured on the ESP32-S3 (docs/PERFORMANCE.md, "MIDI import load"), and a lighter built-in
// instrument for a part when the song would not fit. An estimate, not a measurement: the import
// dialog says so (tested in load.test.ts).
import type { Instrument } from './gm.ts';
import type { MidiNote } from './smf.ts';

/** the most notes of a part that sound at the same time (its voices) */
export function voicesNeeded(notes: MidiNote[]): number {
  const ev: [number, number][] = [];
  for (const n of notes) ev.push([n.start, 1], [n.start + n.len, -1]);
  ev.sort((a, b) => a[0] - b[0] || a[1] - b[1]); // an end before a start at the same tick
  let cur = 0;
  let max = 0;
  for (const e of ev) max = Math.max(max, (cur += e[1]));
  return Math.max(1, max);
}

const MACRO = 'org.smhgzll.macrosynth';
const SOUNDFONT = 'org.smhgzll.soundfont';

/** a SoundFont part's voices. They are samples, not notes: a GM preset can layer several samples
 *  per note and ring on in its release (TimGM6mb's String Ensemble 1: five samples, 2.6 s), so a
 *  part with four notes at once kept all of its 24 voices busy. A part uses about as many as it is
 *  given while its notes keep coming: the import gives each a share by its notes (`sf2Voices`), and
 *  fewer, down to `sf2MinVoices`, when the song would not fit. A note finding no free voice takes
 *  the oldest one (the quietest release first). */
export const sf2Voices = (midiVoices: number, drums: boolean) => Math.min(24, Math.max(drums ? 8 : 6, 4 * midiVoices));
export const sf2MinVoices = (midiVoices: number) => Math.min(24, Math.max(4, 2 * midiVoices));
// % of the audio deadline for one instance: [base, per voice]. Measured with an imported song on
// the board (2026-10-12): MacroSynth Plucked String 2 / 3 / 4 voices 24 / 31 / 36 %, FM6 Electric
// Piano 2 voices 24 %, 3xOSC Pluck 2 voices 13 %, Sine Pluck 2 voices 5.6 %, DrumLab up to 4
// voices 27-40 %; Sampler from the 16 / 24-voice catalog fixtures; SoundFont from an imported
// GM song (a SoundFont part's voices are its sample voices: sf2Voices).
const COST: Record<string, [number, number]> = {
  [MACRO]: [11, 6.5],
  'org.smhgzll.fm6': [2, 11],
  'org.smhgzll.3xosc': [1, 6.5],
  'org.smhgzll.sine': [0.5, 2.8],
  'org.smhgzll.drumlab': [2, 8],
  // TimGM6mb on the board (2026-10-13, faster voice loop): String Ensemble 24 voices 23.6 %,
  // 16 voices 16.1 %, 12 voices 12.5 %; a nine-part GM song 35.3 / 44.9 % mean / max (the
  // estimate gives 61 %), per busy voice (SF2_OCCUPANCY scales the voices given)
  'org.smhgzll.soundfont': [1, 1.1],
  'org.smhgzll.sampler': [1, 3.5],
  'org.smhgzll.acid303': [13, 0],
  // effects: the catalog fixture's share less the Sine source and the engine (2026-10-11)
  'org.smhgzll.fx.distortion': [8, 0],
};
/** the block's fixed work: mixer, master, output */
export const ENGINE_PERCENT = 20;
/** the device admits new instruments below this load (CONFIG_DAW_CPU_ADMIT_PERMILLE) */
export const LOAD_LIMIT = 75;

/** the share of its voices a SoundFont part keeps busy at its busiest: a GM song's parts on the
 *  board (2026-10-13) from a bass at 2 of 8 to strings at 16 of 16, 0.62 over all nine */
export const SF2_OCCUPANCY = 0.6;

export const partCost = (plugin: string, voices: number) => {
  const c = COST[plugin] ?? [5, 6.5];
  return c[0] + c[1] * voices * (plugin === SOUNDFONT ? SF2_OCCUPANCY : 1);
};

/** estimated load in % of the audio deadline: parts share the two cores (an instrument and its
 *  insert effects run on one), so the longest part bounds it too */
export function estimateLoad(parts: { plugin: string; voices: number; fx?: string[] }[]): number {
  let sum = 0;
  let max = 0;
  for (const p of parts) {
    let c = partCost(p.plugin, p.voices);
    for (const f of p.fx ?? []) c += partCost(f, 0);  // an insert runs after its instrument
    sum += c;
    max = Math.max(max, c);
  }
  return ENGINE_PERCENT + Math.max(sum / 2, max);
}

// a lighter built-in instrument with a similar role, or null
const LIGHTER: Record<string, Record<string, Instrument>> = {
  [MACRO]: {
    'Plucked String': { plugin: 'org.smhgzll.3xosc', preset: 'Pluck' },
    'Vowel Pad': { plugin: 'org.smhgzll.3xosc', preset: 'Warm Pad' },
    'Saw Swarm': { plugin: 'org.smhgzll.3xosc', preset: 'Super Saw' },
    'Struck Bell': { plugin: 'org.smhgzll.sine', preset: 'Pluck' },
    'FM Bass': { plugin: 'org.smhgzll.3xosc', preset: 'Fat Bass' },
    '': { plugin: 'org.smhgzll.3xosc', preset: 'Init' },
  },
  'org.smhgzll.fm6': {
    'Electric Piano': { plugin: 'org.smhgzll.3xosc', preset: 'Pluck' },
    'Tubular Bell': { plugin: 'org.smhgzll.sine', preset: 'Pluck' },
    'Drawbar Organ': { plugin: 'org.smhgzll.3xosc', preset: 'Warm Pad' },
    Brass: { plugin: 'org.smhgzll.3xosc', preset: 'Super Saw' },
    'FM Bass': { plugin: 'org.smhgzll.3xosc', preset: 'Fat Bass' },
    '': { plugin: 'org.smhgzll.3xosc', preset: 'Init' },
  },
  'org.smhgzll.3xosc': {
    Pluck: { plugin: 'org.smhgzll.sine', preset: 'Pluck' },
    'Fat Bass': { plugin: 'org.smhgzll.sine', preset: 'Sub' },
    'Warm Pad': { plugin: 'org.smhgzll.sine', preset: 'Soft Pad' },
    'Super Saw': { plugin: 'org.smhgzll.sine', preset: 'Soft Pad' },
    '': { plugin: 'org.smhgzll.sine', preset: 'Init' },
  },
};
export function lighter(i: Instrument): Instrument | null {
  const m = LIGHTER[i.plugin];
  return m ? m[i.preset ?? ''] ?? m[''] : null;
}

/** until the estimate fits, one step at a time: fewer voices for the SoundFont part with the most
 *  (two at a time, down to its `minVoices`), then a lighter instrument for the costliest automatic
 *  part, then without its automatic effects; returns the indexes changed */
export function fitLoad(
  parts: { plugin: string; preset?: string; voices: number; minVoices?: number; auto: boolean; fx?: string[] }[],
  available: (plugin: string) => boolean,
): number[] {
  const changed: number[] = [];
  const cost = (k: number) => partCost(parts[k].plugin, parts[k].voices);
  for (let guard = 0; guard < parts.length * 16 && estimateLoad(parts) > LOAD_LIMIT; guard++) {
    let best = -1;
    for (let k = 0; k < parts.length; k++) {
      const p = parts[k];
      if (p.plugin === SOUNDFONT && p.minVoices !== undefined && p.voices > p.minVoices && (best < 0 || p.voices > parts[best].voices)) best = k;
    }
    if (best >= 0) {
      parts[best].voices = Math.max(parts[best].minVoices!, parts[best].voices - 2);
      if (!changed.includes(best)) changed.push(best);
      continue;
    }
    for (let k = 0; k < parts.length; k++) {
      const l = parts[k].auto ? lighter(parts[k]) : null;
      if (l && available(l.plugin) && (best < 0 || cost(k) > cost(best))) best = k;
    }
    if (best < 0) {
      // no lighter instrument left: the automatic effects go, the costliest part's first
      for (let k = 0; k < parts.length; k++)
        if (parts[k].auto && parts[k].fx?.length && (best < 0 || cost(k) > cost(best))) best = k;
      if (best < 0) break;
      parts[best].fx = [];
    } else {
      const l = lighter(parts[best])!;
      parts[best].plugin = l.plugin;
      parts[best].preset = l.preset;
    }
    if (!changed.includes(best)) changed.push(best);
  }
  return changed;
}
