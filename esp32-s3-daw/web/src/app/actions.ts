// SPDX-License-Identifier: GPL-3.0-or-later
// Every edit of the UI goes through here: an optimistic local change (instant feedback), the
// protocol request (the device validates and broadcasts the authoritative patch), and an undo entry.
import { record } from './history';
import { req, send, sendControl } from './protocol';
import { catalog, channelById, currentPattern, live, P, patternById, replacePattern, samples, toast, ui, updateProject } from './state';
import type { Clip, Note, Pattern, Seq, StepArr } from './types';
import { fromNote, toNote } from './types';

// ---- transport ---------------------------------------------------------------------------------
export const play = () => send('play');
export const stop = () => send('stop');
export const togglePlay = () => send('toggle');
export const setMode = (mode: 'pattern' | 'song') => send('mode', { mode });
export const setBpm = (bpm: number, final: boolean) => {
  updateProject((p) => ({ ...p, meta: { ...p.meta, bpm } }));
  sendControl('bpm', 'bpm', { bpm }, final);
};
export const seek = (tick: number) => send('seek', { tick: Math.max(0, Math.round(tick)) });
export const setLoop = (on: boolean, start?: number, end?: number) => send('loop', { on, ...(start !== undefined ? { start, end } : {}) });
export const setMetronome = (on: boolean) => send('metronome', { on });
export const setCountIn = (bars: number) => send('metronome', { countIn: bars });
export const panic = () => send('panic');

let taps: number[] = [];
export function tapTempo() {
  const now = performance.now();
  taps = taps.filter((t) => now - t < 2500);
  taps.push(now);
  if (taps.length >= 3) {
    const iv = (taps[taps.length - 1] - taps[0]) / (taps.length - 1);
    setBpm(Math.round(60000 / iv), true);
  }
}

// ---- patterns ------------------------------------------------------------------------------------
export const selectPattern = (id: number, queue = true) => {
  updateProject((p) => ({ ...p, meta: { ...p.meta, currentPattern: id } }));
  send('pattern.select', { pattern: id, queue: queue && live.state.status?.play === 1 });
};
export function stepPattern(dir: number) {
  const p = P();
  if (!p) return;
  const i = p.patterns.findIndex((x) => x.id === p.meta.currentPattern);
  const n = p.patterns[(i + dir + p.patterns.length) % p.patterns.length];
  if (n) selectPattern(n.id);
}
export const addPattern = async (name?: string) => (await req('pattern.add', name ? { name } : {})).newId as number;
export const dupPattern = (id: number) => send('pattern.dup', { pattern: id });
export const deletePattern = (id: number) => send('pattern.del', { pattern: id });
export const setPattern = (id: number, f: Partial<{ name: string; color: string; length: number; swing: number }>) => {
  updateProject((p) => ({ ...p, patterns: p.patterns.map((x) => (x.id === id ? { ...x, ...f } : x)) }));
  sendControl(`pat${id}`, 'pattern.set', { pattern: id, ...f }, true);
};

export function seqOf(pat: Pattern | undefined, channel: number): Seq | undefined {
  return pat?.seqs.find((s) => s.channel === channel);
}
function withSeq(pat: Pattern, channel: number, fn: (s: Seq) => Seq): Pattern {
  const has = pat.seqs.some((s) => s.channel === channel);
  const base: Seq = { channel, stepCount: 16, pianoRoll: false, nextNoteId: 1, steps: emptySteps(16), notes: [] };
  return { ...pat, seqs: has ? pat.seqs.map((s) => (s.channel === channel ? fn(s) : s)) : [...pat.seqs, fn(base)] };
}

// ---- steps -------------------------------------------------------------------------------------
/** a row of steps that are all off, with the default velocity / probability */
export const emptySteps = (n: number) => Array.from({ length: n }, () => [0, 0.78, 0, 0, 0, 1] as StepArr);

export function setSteps(patId: number, channel: number, steps: StepArr[], label = 'Edit steps', count?: number) {
  const pat = patternById(patId);
  if (!pat) return;
  const before = seqOf(pat, channel);
  const prevSteps = before?.steps.map((s) => [...s] as StepArr) ?? emptySteps(16);
  const prevCount = before?.stepCount ?? 16;
  const apply = (st: StepArr[], c: number) => {
    const p = patternById(patId);
    if (p) replacePattern(withSeq(p, channel, (s) => ({ ...s, steps: st, stepCount: c })));
    send('steps.set', { pattern: patId, channel, steps: st, count: c });
  };
  const n = count ?? steps.length;
  apply(steps, n);
  record(label, () => apply(prevSteps, prevCount), () => apply(steps, n));
}
export function toggleStep(patId: number, channel: number, index: number, on?: boolean, accent?: boolean) {
  const pat = patternById(patId);
  const s = seqOf(pat, channel);
  const steps = (s?.steps ?? emptySteps(16)).map((x) => [...x] as StepArr);
  if (index >= steps.length) return;
  steps[index][0] = on === undefined ? (steps[index][0] ? 0 : 1) : on ? 1 : 0;
  if (accent !== undefined) steps[index][4] = accent ? 1 : 0;
  setSteps(patId, channel, steps, 'Toggle step', s?.stepCount ?? steps.length);
}
export function setStepCount(patId: number, channel: number, count: number) {
  const s = seqOf(patternById(patId), channel);
  const steps = (s?.steps ?? []).map((x) => [...x] as StepArr);
  while (steps.length < count) steps.push([0, 0.78, 0, 0, 0, 1]);
  setSteps(patId, channel, steps.slice(0, count), 'Step count', count);
}
export function stepOp(patId: number, channel: number, op: 'clear' | 'fill2' | 'fill4' | 'fill8' | 'shiftL' | 'shiftR' | 'random' | 'invert') {
  const s = seqOf(patternById(patId), channel);
  const steps = (s?.steps ?? emptySteps(16)).map((x) => [...x] as StepArr);
  const n = steps.length;
  switch (op) {
    case 'clear': steps.forEach((x) => (x[0] = 0)); break;
    case 'fill2': steps.forEach((x, i) => (x[0] = i % 2 === 0 ? 1 : 0)); break;
    case 'fill4': steps.forEach((x, i) => (x[0] = i % 4 === 0 ? 1 : 0)); break;
    case 'fill8': steps.forEach((x, i) => (x[0] = i % 8 === 0 ? 1 : 0)); break;
    case 'invert': steps.forEach((x) => (x[0] = x[0] ? 0 : 1)); break;
    case 'random': steps.forEach((x) => (x[0] = Math.random() < 0.35 ? 1 : 0)); break;
    case 'shiftL': steps.push(steps.shift()!); break;
    case 'shiftR': steps.unshift(steps.pop()!); break;
  }
  setSteps(patId, channel, steps.slice(0, n), 'Step operation', s?.stepCount ?? n);
}
let stepClipboard: StepArr[] | null = null;
export const copySteps = (patId: number, channel: number) => {
  stepClipboard = seqOf(patternById(patId), channel)?.steps.map((x) => [...x] as StepArr) ?? null;
};
export const pasteSteps = (patId: number, channel: number) => {
  if (stepClipboard) setSteps(patId, channel, stepClipboard.map((x) => [...x] as StepArr), 'Paste steps');
};

// ---- notes (piano roll) ---------------------------------------------------------------------------
export function notesOf(patId: number, channel: number): Note[] {
  return (seqOf(patternById(patId), channel)?.notes ?? []).map(toNote);
}
/** replace a row's notes (the piano roll computes every edit locally, then sends the result) */
export function setNotes(patId: number, channel: number, notes: Note[], label: string, before?: Note[], midi?: number[][]) {
  const prev = before ?? notesOf(patId, channel);
  const prevMidi = seqOf(patternById(patId), channel)?.midi ?? [];
  const apply = (ns: Note[], events = midi) => {
    const p = patternById(patId);
    if (p) replacePattern(withSeq(p, channel, (s) => ({ ...s, notes: ns.map(fromNote), pianoRoll: true, ...(events !== undefined ? { midi: events } : {}) })));
    send('notes.set', { pattern: patId, channel, notes: ns.map(fromNote), ...(events !== undefined ? { midi: events } : {}) });
  };
  apply(notes);
  record(label, () => apply(prev, midi !== undefined ? prevMidi : undefined), () => apply(notes));
}
export const setSeqMode = (patId: number, channel: number, pianoRoll: boolean) => send('seq.mode', { pattern: patId, channel, pianoRoll, convert: true });

// ---- channels ------------------------------------------------------------------------------------
export async function addChannel(plugin: string, name?: string, preset?: string) {
  const info = catalog.state.plugins.find((p) => p.id === plugin);
  try {
    const a = await req('channel.add', { plugin, name: name ?? info?.name, ...(preset ? { preset } : {}) });
    ui.set({ selectedChannel: a.newId as number });
    return a.newId as number;
  } catch (e) {
    toast(`Cannot add ${plugin}: ${(e as Error).message}`, 'error');
    return 0;
  }
}
export const deleteChannel = (id: number) => send('channel.del', { channel: id });
export const setChannel = (id: number, f: Record<string, unknown>, final = true) => {
  updateProject((p) => ({ ...p, channels: p.channels.map((c) => (c.id === id ? ({ ...c, ...f } as typeof c) : c)) }));
  if ('vol' in f || 'pan' in f) sendControl(`ch${id}`, 'channel.set', { channel: id, ...f }, final);
  else send('channel.set', { channel: id, ...f });
};
export const moveChannel = (id: number, index: number) => send('channel.move', { channel: id, index });
export const setChannelPlugin = (id: number, plugin: string, preset?: string) => send('channel.plugin', { channel: id, plugin, ...(preset ? { preset } : {}) });
export function setChannelParam(id: number, key: string, v: number, final: boolean) {
  updateProject((p) => ({
    ...p,
    channels: p.channels.map((c) => (c.id === id ? { ...c, plugin: { ...c.plugin, params: { ...c.plugin.params, [key]: v } } } : c)),
  }));
  sendControl(`chp${id}.${key}`, 'channel.param', { channel: id, key, v }, final);
}
export const loadChannelPreset = (id: number, name: string) => send('channel.preset', { channel: id, name });

// ---- live notes (piano keys, virtual keyboard, step preview) ----------------------------------------
const held = new Set<string>();
export function noteOn(channel: number, key: number, vel = 0.8) {
  const k = `${channel}:${key}`;
  if (held.has(k)) return;
  held.add(k);
  send('note', { channel, key, vel, on: true });
}
export function noteOff(channel: number, key: number) {
  const k = `${channel}:${key}`;
  if (!held.delete(k)) return;
  send('note', { channel, key, on: false });
}
export function previewNote(channel: number, key: number, ms = 220) {
  noteOn(channel, key, 0.8);
  setTimeout(() => noteOff(channel, key), ms);
}

// ---- mixer -----------------------------------------------------------------------------------------
export function setTrack(index: number, f: Record<string, unknown>, final = true) {
  updateProject((p) => ({ ...p, mixer: p.mixer.map((t) => (t.index === index ? ({ ...t, ...f } as typeof t) : t)) }));
  if (('vol' in f || 'pan' in f) && Object.keys(f).length <= 2) sendControl(`tr${index}`, 'track.set', { track: index, ...f }, final);
  else send('track.set', { track: index, ...f });
}
export const setSend = (track: number, slot: number, target: number, level: number, final = true) =>
  sendControl(`send${track}.${slot}`, 'track.send', { track, slot, target, level }, final);
export const addFx = (track: number, plugin: string, index?: number) => send('fx.add', { track, plugin, ...(index !== undefined ? { index } : {}) });
export const removeFx = (fx: number) => send('fx.del', { fx });
export const moveFx = (fx: number, index: number) => send('fx.move', { fx, index });
export const setFx = (fx: number, f: { enabled?: boolean; mix?: number }) => send('fx.set', { fx, ...f });
export function setFxParam(fx: number, key: string, v: number, final: boolean) {
  updateProject((p) => ({
    ...p,
    mixer: p.mixer.map((t) => ({ ...t, fx: t.fx.map((x) => (x.id === fx ? { ...x, plugin: { ...x.plugin, params: { ...x.plugin.params, [key]: v } } } : x)) })),
  }));
  sendControl(`fxp${fx}.${key}`, 'fx.param', { fx, key, v }, final);
}
export const loadFxPreset = (fx: number, name: string) => send('fx.preset', { fx, name });

// ---- playlist ----------------------------------------------------------------------------------------
export function setClips(clips: Clip[], label: string, before?: Clip[]) {
  const prev = before ?? P()?.playlist.clips ?? [];
  const apply = (cs: Clip[]) => {
    updateProject((p) => ({ ...p, playlist: { ...p.playlist, clips: cs } }));
    send('clips.set', { clips: cs });
  };
  apply(clips);
  record(label, () => apply(prev), () => apply(clips));
}
export const setPlaylistTrack = (index: number, f: Record<string, unknown>) => send('ptrack.set', { index, ...f });
export const addPlaylistTrack = () => send('ptrack.set', { add: true });

// ---- automation ----------------------------------------------------------------------------------------
/** a new automation clip, flat at the target's current value (normalised 0..1), opened for editing */
export async function automateParam(target: Record<string, unknown>, name: string, start = 0.5) {
  const v = Math.min(1, Math.max(0, Number.isFinite(start) ? start : 0.5));
  const a = await req('auto.add', { name, target, points: [[0, v, 0], [3840 * 2, v, 0]], length: 3840 * 4 });
  const p = P();
  const track = p ? p.playlist.tracks.length - 1 : 0;
  await req('clip.add', { kind: 'automation', ref: a.newId, track, start: 0 });
  toast(`Automation clip "${name}" added to the playlist (track ${track + 1})`);
  ui.set({ autoEdit: a.newId as number });
}
export const setAutomation = (id: number, f: Record<string, unknown>) => send('auto.set', { auto: id, ...f });
/** replace an automation clip's points (one undo step) */
export function setAutomationPoints(id: number, points: [number, number, number][], label: string, before?: [number, number, number][]) {
  const prev = before ?? P()?.automation.find((x) => x.id === id)?.points ?? [];
  const apply = (pts: [number, number, number][]) => {
    updateProject((p) => ({ ...p, automation: p.automation.map((x) => (x.id === id ? { ...x, points: pts } : x)) }));
    send('auto.set', { auto: id, points: pts });
  };
  apply(points);
  record(label, () => apply(prev), () => apply(points));
}

// ---- projects ------------------------------------------------------------------------------------------
export const newProject = (template: 'demo' | 'empty') => send('project.new', { template });
export async function saveProject(slot: number, name?: string) {
  try {
    const a = await req('project.save', { slot, ...(name ? { name } : {}) });
    toast(`Saved to slot ${slot} (${Math.round((a.bytes as number) / 1024)} KB)`);
  } catch {
    /* toast from the error message */
  }
}
export const loadProject = (slot: number) => send('project.load', { slot });
export const deleteProject = (slot: number) => send('project.delete', { slot });
export const renameProject = (name: string) => send('project.rename', { name });
export const diagTone = (kind: string) => send('diag.tone', { kind });

export const selectedChannel = () => channelById(ui.state.selectedChannel);
export const curPattern = currentPattern;

// ---- the sample library (ABI 1.1 resources) ------------------------------------------------------------
/** upload files into the device's sample library (one request each; the device refuses while playing) */
export async function uploadSamples(files: FileList | File[]) {
  for (const f of Array.from(files)) {
    try {
      const r = await fetch(`/api/samples?name=${encodeURIComponent(f.name)}`, { method: 'POST', body: f });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(j.error ?? r.statusText);
      samples.set({ library: j });
      toast(`${f.name}: in the sample library (${Math.round(f.size / 1024)} KB)`);
    } catch (e) {
      toast(`${f.name}: ${(e as Error).message}`, 'error');
    }
  }
}
export const deleteSample = (name: string) => send('samples.delete', { name });
export const setChannelResource = (channel: number, slot: string, sample: string) => send('channel.resource', { channel, slot, sample });
/** a new Sampler channel playing this file (or a SoundFont player for .sf2) */
export async function addSampleChannel(name: string) {
  const sf2 = name.toLowerCase().endsWith('.sf2');
  const plugin = catalog.state.plugins.find((p) => p.resourceSlots?.some((s) => (sf2 ? s.file && s.extensions.includes('sf2') : s.audio)));
  if (!plugin) {
    toast(`No plugin in this firmware plays ${sf2 ? 'SoundFonts' : 'audio files'}`, 'error');
    return;
  }
  const id = await addChannel(plugin.id, name.replace(/\.[^.]+$/, '').slice(0, 24));
  if (!id) return;
  await req('channel.resource', { channel: id, slot: plugin.resourceSlots![0].key, sample: name }).catch(() => undefined);
  // open its editor right away (like dropping a sample on the channel rack)
  ui.set((s) => ({ selectedChannel: id, editor: { kind: 'channel', id }, panels: { ...s.panels, plugin: !s.panels.settings } }));
}

// ---- user presets (stored on the device) -------------------------------------------------------------
export async function saveUserPreset(target: { channel: number } | { fx: number }, suggested: string) {
  const name = prompt('Preset name (saved on the device)', suggested);
  if (!name) return;
  try {
    await req('preset.save', { ...target, name });
    toast(`Preset "${name}" saved`);
  } catch {
    /* the error toast says why */
  }
}
export const deleteUserPreset = (plugin: string, name: string) => send('preset.delete', { plugin, name });

// ---- MIDI recording ------------------------------------------------------------------------------------
export const RECORD_GRIDS: [string, number][] = [['As played', 0], ['1/32', 60], ['1/16 (step)', 240], ['1/8', 480], ['1/4 (beat)', 960]];
export async function setRecord(on: boolean, quantize?: number) {
  const st = live.state.status;
  if (on && st?.mode === 'song') toast('Recording works in pattern mode: notes go into the current pattern', 'info');
  await req('record', { on, ...(quantize !== undefined ? { quantize } : {}) }).catch(() => undefined);
}
