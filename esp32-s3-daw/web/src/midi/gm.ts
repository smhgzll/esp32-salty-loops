// SPDX-License-Identifier: GPL-3.0-or-later
// General MIDI: program names, the built-in instrument chosen for each program when a MIDI file
// is imported without a SoundFont, and GM percussion keys mapped onto DrumLab's pieces.

export const GM_PROGRAMS = [
  'Acoustic Grand Piano', 'Bright Acoustic Piano', 'Electric Grand Piano', 'Honky-tonk Piano', 'Electric Piano 1', 'Electric Piano 2', 'Harpsichord', 'Clavinet',
  'Celesta', 'Glockenspiel', 'Music Box', 'Vibraphone', 'Marimba', 'Xylophone', 'Tubular Bells', 'Dulcimer',
  'Drawbar Organ', 'Percussive Organ', 'Rock Organ', 'Church Organ', 'Reed Organ', 'Accordion', 'Harmonica', 'Tango Accordion',
  'Acoustic Guitar (nylon)', 'Acoustic Guitar (steel)', 'Electric Guitar (jazz)', 'Electric Guitar (clean)', 'Electric Guitar (muted)', 'Overdriven Guitar', 'Distortion Guitar', 'Guitar Harmonics',
  'Acoustic Bass', 'Electric Bass (finger)', 'Electric Bass (pick)', 'Fretless Bass', 'Slap Bass 1', 'Slap Bass 2', 'Synth Bass 1', 'Synth Bass 2',
  'Violin', 'Viola', 'Cello', 'Contrabass', 'Tremolo Strings', 'Pizzicato Strings', 'Orchestral Harp', 'Timpani',
  'String Ensemble 1', 'String Ensemble 2', 'Synth Strings 1', 'Synth Strings 2', 'Choir Aahs', 'Voice Oohs', 'Synth Voice', 'Orchestra Hit',
  'Trumpet', 'Trombone', 'Tuba', 'Muted Trumpet', 'French Horn', 'Brass Section', 'Synth Brass 1', 'Synth Brass 2',
  'Soprano Sax', 'Alto Sax', 'Tenor Sax', 'Baritone Sax', 'Oboe', 'English Horn', 'Bassoon', 'Clarinet',
  'Piccolo', 'Flute', 'Recorder', 'Pan Flute', 'Blown Bottle', 'Shakuhachi', 'Whistle', 'Ocarina',
  'Lead 1 (square)', 'Lead 2 (sawtooth)', 'Lead 3 (calliope)', 'Lead 4 (chiff)', 'Lead 5 (charang)', 'Lead 6 (voice)', 'Lead 7 (fifths)', 'Lead 8 (bass + lead)',
  'Pad 1 (new age)', 'Pad 2 (warm)', 'Pad 3 (polysynth)', 'Pad 4 (choir)', 'Pad 5 (bowed)', 'Pad 6 (metallic)', 'Pad 7 (halo)', 'Pad 8 (sweep)',
  'FX 1 (rain)', 'FX 2 (soundtrack)', 'FX 3 (crystal)', 'FX 4 (atmosphere)', 'FX 5 (brightness)', 'FX 6 (goblins)', 'FX 7 (echoes)', 'FX 8 (sci-fi)',
  'Sitar', 'Banjo', 'Shamisen', 'Koto', 'Kalimba', 'Bagpipe', 'Fiddle', 'Shanai',
  'Tinkle Bell', 'Agogo', 'Steel Drums', 'Woodblock', 'Taiko Drum', 'Melodic Tom', 'Synth Drum', 'Reverse Cymbal',
  'Guitar Fret Noise', 'Breath Noise', 'Seashore', 'Bird Tweet', 'Telephone Ring', 'Helicopter', 'Applause', 'Gunshot',
];

export interface Instrument {
  plugin: string;
  preset?: string;
}

const FM6 = 'org.smhgzll.fm6';
const OSC3 = 'org.smhgzll.3xosc';
const MACRO = 'org.smhgzll.macrosynth';
const SINE = 'org.smhgzll.sine';
export const DRUMLAB = 'org.smhgzll.drumlab';
export const SOUNDFONT = 'org.smhgzll.soundfont';
export const DISTORTION = 'org.smhgzll.fx.distortion';

export interface Effect {
  plugin: string;
  params: Record<string, number>; // parameter key -> plain value
  label: string;
}
/** effects on a part's mixer insert that bring a built-in instrument closer to the GM original:
 *  the overdriven and distortion guitars are a plucked string through a clipper and a cabinet-like
 *  low-pass (Distortion's tone) */
export function builtinEffects(program: number, drums: boolean): Effect[] {
  if (drums) return [];
  if (program === 29) return [{ plugin: DISTORTION, params: { mode: 0, drive: 14, tone: 6000, output: -8, mix: 100 }, label: 'Overdrive' }];
  if (program === 30) return [{ plugin: DISTORTION, params: { mode: 1, drive: 24, tone: 4500, output: -12, mix: 100 }, label: 'Distortion' }];
  return [];
}

/** the built-in instrument for a GM program (-1 = none given: GM's default, the piano) */
export function builtinInstrument(program: number, drums: boolean): Instrument {
  if (drums) return { plugin: DRUMLAB, preset: 'Default Kit' };
  const p = program < 0 ? 0 : program;
  if (p === 45 || p === 46) return { plugin: MACRO, preset: 'Plucked String' }; // pizzicato, harp
  if (p >= 52 && p <= 54) return { plugin: MACRO, preset: 'Vowel Pad' }; // voices
  if (p === 55 || p === 81) return { plugin: OSC3, preset: 'Super Saw' };
  if (p >= 109 && p <= 111) return { plugin: OSC3, preset: 'Mono Lead' }; // bagpipe, fiddle, shanai
  switch (p >> 3) {
    case 0: return { plugin: FM6, preset: 'Electric Piano' };
    case 1: return { plugin: FM6, preset: 'Tubular Bell' };
    case 2: return { plugin: FM6, preset: 'Drawbar Organ' };
    case 3: return { plugin: MACRO, preset: 'Plucked String' };
    case 4: return { plugin: OSC3, preset: 'Fat Bass' };
    case 5:
    case 6:
    case 11: return { plugin: OSC3, preset: 'Warm Pad' };
    case 7: return { plugin: FM6, preset: 'Brass' };
    case 8:
    case 10: return { plugin: OSC3, preset: 'Mono Lead' };
    case 9: return { plugin: SINE, preset: 'Soft Pad' };
    case 12: return { plugin: MACRO, preset: 'Saw Swarm' };
    case 13: return { plugin: MACRO, preset: 'Plucked String' };
    case 14: return { plugin: MACRO, preset: 'Struck Bell' };
    default: return { plugin: SINE, preset: 'Init' };
  }
}

// GM percussion keys DrumLab has no piece for, played by the closest one it has (DrumLab plays
// GM keys 35-48 and 50 in its Kit mode). Keys missing here and in DrumLab are left out.
const DRUMLAB_KEYS = new Set([35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 50]);
const TO_DRUMLAB: Record<number, number> = {
  31: 37, 32: 37, 33: 37, 34: 37, // sticks, clicks, bell -> rim
  49: 46, 52: 46, 55: 46, 57: 46, // crashes, china, splash -> open hat
  51: 42, 53: 42, 59: 42, // rides -> closed hat
  54: 42, 69: 42, 70: 42, 73: 42, 74: 42, 80: 44, 81: 46, 82: 42, // tambourine, shakers, guiro, triangles
  56: 37, 58: 37, 67: 37, 68: 37, 75: 37, 76: 37, 77: 37, // cowbell, vibraslap, agogo, claves, woodblocks -> rim
  60: 50, 61: 48, 62: 47, 63: 45, 64: 43, 65: 48, 66: 45, // bongos, congas, timbales -> toms
};
/** the DrumLab key for a GM percussion key, or null when it has nothing close */
export function drumLabKey(key: number): number | null {
  if (DRUMLAB_KEYS.has(key)) return key;
  return TO_DRUMLAB[key] ?? null;
}
