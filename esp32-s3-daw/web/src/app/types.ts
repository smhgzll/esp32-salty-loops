// SPDX-License-Identifier: GPL-3.0-or-later
// The protocol's JSON shapes (docs/PROTOCOL.md). Compact arrays mirror the project file format.

/** [id, start, len, key, vel, pan, pitch, rel, prob, flags(1 slide, 2 accent), midiOnOrder?, midiOffOrder?] */
export type NoteArr = [number, number, number, number, number, number, number, number, number, number, number?, number?];
/** [on, vel, pan, key(0 = channel root), accent, prob] */
export type StepArr = [number, number, number, number, number, number];

export interface Seq {
  channel: number;
  stepCount: number;
  pianoRoll: boolean;
  nextNoteId: number;
  steps: StepArr[];
  notes: NoteArr[];
  midi?: number[][]; // [tick, original order, status, data...]
}
export interface Pattern {
  id: number;
  name: string;
  color: string;
  length: number;
  effLength: number;
  swing: number;
  seqs: Seq[];
}
export interface PluginState {
  id: string;
  version?: string;
  params: Record<string, number>;
  preset?: string;
  blob?: string;
  resources?: Record<string, string>; // ABI 1.1: slot key -> sample library name
  missingResources?: string[];
  loadError?: string;
}
export interface ResourceSlot {
  key: string;
  name: string;
  audio: boolean;
  file: boolean;
  extensions: string;
}
export interface SampleFile {
  name: string;
  size: number;
  crc?: number;
  type: 'wav' | 'sf2' | 'module' | '?';
}
export interface SampleLibrary {
  list: SampleFile[];
  total?: number;
  free?: number;
  largest?: number;
}
export interface Channel {
  id: number;
  name: string;
  color: string;
  icon: string;
  vol: number;
  pan: number;
  mute: boolean;
  solo: boolean;
  track: number;
  root: number;
  plugin: PluginState;
  missing?: boolean;
}
export interface FxSlot {
  id: number;
  enabled: boolean;
  mix: number;
  plugin: PluginState;
  missing?: boolean;
}
export interface Send {
  target: number;
  level: number;
}
export interface Track {
  index: number;
  name: string;
  color: string;
  vol: number;
  pan: number;
  mute: boolean;
  solo: boolean;
  route: number;
  sends: Send[];
  fx: FxSlot[];
}
export type ClipKind = 'pattern' | 'automation' | 'audio';
export interface Clip {
  id: number;
  kind: ClipKind;
  track: number;
  start: number;
  len: number;
  offset: number;
  ref: number;
  muted?: boolean;
}
export interface PTrack {
  name: string;
  color: string;
  mute: boolean;
  solo: boolean;
}
export interface Marker {
  id: number;
  tick: number;
  name: string;
}
export interface Playlist {
  tracks: PTrack[];
  clips: Clip[];
  markers: Marker[];
}
export type TargetKind = 'none' | 'channelVol' | 'channelPan' | 'channelParam' | 'trackVol' | 'trackPan' | 'fxParam' | 'fxMix' | 'sendLevel' | 'tempo';
export interface AutoTarget {
  kind: TargetKind;
  channel: number;
  track: number;
  fx: number;
  send: number;
  param: string;
}
export interface Automation {
  id: number;
  name: string;
  color: string;
  length: number;
  target: AutoTarget;
  points: [number, number, number][];
}
export interface Meta {
  name: string;
  author: string;
  bpm: number;
  timeSig: [number, number];
  loop: { on: boolean; start: number; end: number };
  metronome: boolean;
  countIn: number;
  currentPattern: number;
  ppq: number;
}
export interface Project {
  meta: Meta;
  channels: Channel[];
  patterns: Pattern[];
  mixer: Track[];
  playlist: Playlist;
  automation: Automation[];
}
export interface ParamDesc {
  id: number;
  key: string;
  name: string;
  group: string;
  unit: string;
  type: 'float' | 'int' | 'bool' | 'enum';
  scale: 'linear' | 'log' | 'db' | 'bipolar';
  min: number;
  max: number;
  def: number;
  auto: boolean;
  labels?: string[];
}
export interface PluginInfo {
  id: string;
  name: string;
  vendor: string;
  version: string;
  license: string;
  description: string;
  category: string;
  type: 'instrument' | 'effect';
  maxVoices: number;
  capabilities: Record<string, boolean>;
  resources: Record<string, number>;
  params: ParamDesc[];
  presets: { name: string; category: string }[];
  keyNames?: Record<string, string>;
  resourceSlots?: ResourceSlot[];
}
export interface ServerInfo {
  name: string;
  version: string;
  target: string;
  sampleRate: number;
  blockFrames: number;
  ppq: number;
  limits: Record<string, number>;
}
export interface Status {
  play: number; // 0 stopped, 1 playing, 2 paused, 3 count-in
  mode: 'pattern' | 'song';
  tick: number;
  pattern: number;
  queued: number;
  bpm: number;
  cpu: number;
  voices: number;
  ext: boolean;
  rec?: boolean; // MIDI recording armed
  cpuSkips?: number; // note-ons skipped by the CPU guard since boot
  m: number[]; // mixer meters L,R per track
  cm: number[]; // channel meters, channel order
}
export interface PluginLoad {
  where: 'channel' | 'fx';
  id: number;
  plugin: string;
  cpu: number;
  voices: number;
  memFast: number;
  memLarge: number;
  reports: number;
}
export interface Diag {
  cpu: number;
  renderUsAvg: number;
  renderUsMax: number;
  budgetUs: number;
  overruns: number;
  eventDrops: number;
  noteDrops: number;
  cpuGuardDrops?: number; // note-ons skipped by the CPU guard
  load?: number; // render load smoothed over ~50 ms, %
  nanResets: number;
  voices: number;
  plugins: PluginLoad[];
  [k: string]: unknown;
}
export interface SlotInfo {
  slot: number;
  used: boolean;
  autosave: boolean;
  name?: string;
  bytes?: number;
  time?: number;
}

export interface Note {
  id: number;
  start: number;
  len: number;
  key: number;
  vel: number;
  pan: number;
  pitch: number;
  rel: number;
  prob: number;
  slide: boolean;
  accent: boolean;
  midiOnOrder?: number;
  midiOffOrder?: number;
}

export const toNote = (a: NoteArr): Note => ({
  id: a[0], start: a[1], len: a[2], key: a[3], vel: a[4], pan: a[5], pitch: a[6], rel: a[7], prob: a[8],
  slide: (a[9] & 1) !== 0, accent: (a[9] & 2) !== 0, midiOnOrder: a[10], midiOffOrder: a[11],
});
export const fromNote = (n: Note): NoteArr => [n.id, n.start, n.len, n.key, n.vel, n.pan, n.pitch, n.rel, n.prob, (n.slide ? 1 : 0) | (n.accent ? 2 : 0), n.midiOnOrder ?? 0, n.midiOffOrder ?? 0];
