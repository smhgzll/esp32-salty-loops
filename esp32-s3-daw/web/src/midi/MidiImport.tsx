// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI file import (docs/MIDI.md, "MIDI files"). The browser reads the file (smf.ts) and builds the
// result with the ordinary editing operations, so the device needs no file parser and no
// privileged endpoint:
//   as a project         File > Import MIDI file…: a new project with a channel per part, the parts
//                        cut into patterns (repeats share one) laid out on the playlist, tempo
//                        automation, markers
//   into the piano roll  the piano roll's import button: some parts' notes, or a range of bars of
//                        them, into the channel being edited (one undo step)
import { render } from 'preact';
import { useEffect, useMemo, useState } from 'preact/hooks';

import * as A from '../app/actions';
import { clearHistory } from '../app/history';
import { req } from '../app/protocol';
import { catalog, channelById, conn, P, patternById, samples, toast, ui } from '../app/state';
import { useStore } from '../app/store';
import type { PluginInfo } from '../app/types';
import { Btn, Modal } from '../ui/controls';
import { newNote, nextId } from '../piano-roll/model';
import { arrange, barTicksOf, estimateProjectBytes, estimateStoredBytes, LIMITS, mergeParts, sliceNotes, songTimeSig, tempoAutomation } from './arrange';
import type { Arrangement, AutoPlan, MergeMode } from './arrange';
import { builtinEffects, builtinInstrument, DRUMLAB, drumLabKey, GM_PROGRAMS, SOUNDFONT } from './gm';
import type { Effect } from './gm';
import { estimateLoad, fitLoad, LOAD_LIMIT, partCost, sf2MinVoices, sf2Voices, voicesNeeded } from './load';
import { MidiError, parseMidi } from './smf';
import { midiRows, noteRow, midiPresets, pedalNotes, unmappedMidi, sliceMidi, canonicalMidiOrder } from './expressive';
import type { MidiNote, MidiPart, MidiSong } from './smf';

export const MIDI_ACCEPT = '.mid,.midi,.kar,.smf,audio/midi,audio/x-midi';
export const isMidiFile = (f: File) => /\.(mid|midi|kar|smf)$/i.test(f.name) || f.type === 'audio/midi' || f.type === 'audio/x-midi';

const COLORS = ['#5fb3ff', '#4cd9a0', '#ffb03a', '#9a7bff', '#ff7ad9', '#c6e04a', '#4cc9e0', '#ff8a5c', '#7b8cff', '#f5d547', '#d07bff', '#8fd694'];
const DRUM_COLOR = '#ff6b4a';

let host: HTMLElement | null = null;
function mount(node: preact.ComponentChild) {
  if (!host) {
    host = document.createElement('div');
    document.body.appendChild(host);
  }
  render(node, host);
}
const close = () => host && render(null, host);

function pickFile(): Promise<File | null> {
  return new Promise((resolve) => {
    const inp = document.createElement('input');
    inp.type = 'file';
    inp.accept = MIDI_ACCEPT;
    inp.setAttribute('aria-label', 'MIDI file');
    inp.style.display = 'none';
    document.body.appendChild(inp);
    inp.onchange = () => {
      resolve(inp.files?.[0] ?? null);
      inp.remove();
    };
    inp.addEventListener('cancel', () => {
      resolve(null);
      inp.remove();
    });
    inp.click();
  });
}

/** read a MIDI file (asks for one when not given) and open the import dialog for it */
export async function importMidi(target: 'project' | 'piano', file?: File) {
  if (!conn.state.server?.limits.midiEvents) {
    toast('Update the device firmware to import complete MIDI messages.', 'error'); return;
  }
  const f = file ?? (await pickFile());
  if (!f) return;
  let song: MidiSong;
  try {
    song = parseMidi(new Uint8Array(await f.arrayBuffer()));
  } catch (e) {
    toast(`${f.name}: ${e instanceof MidiError ? e.message : 'the file could not be read'}`, 'error');
    return;
  }
  if (!song.parts.length) {
    toast(`${f.name}: the file has no notes`, 'error');
    return;
  }
  if (target === 'project') mount(<ProjectImport file={f.name} song={song} />);
  else {
    const p = P();
    const pat = p?.patterns.find((x) => x.id === p.meta.currentPattern) ?? p?.patterns[0];
    const ch = channelById(ui.state.pianoChannel) ?? p?.channels[0];
    if (!pat || !ch) {
      toast('Add a channel first: the notes go into the channel the piano roll edits', 'error');
      return;
    }
    mount(<PianoImport file={f.name} song={song} pattern={pat.id} channel={ch.id} />);
  }
}

// ---- shared ------------------------------------------------------------------------------------
const fmtBpm = (b: number) => (Math.round(b * 10) / 10).toString();
const keyName = (k: number) => `${['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'][k % 12]}${Math.floor(k / 12) - 1}`;
function range(ns: MidiNote[]) {
  let lo = 127, hi = 0;
  for (const n of ns) {
    lo = Math.min(lo, n.key);
    hi = Math.max(hi, n.key);
  }
  return lo === hi ? keyName(lo) : `${keyName(lo)}–${keyName(hi)}`;
}
const plural = (n: number, one: string, many = `${one}s`) => `${n} ${n === 1 ? one : many}`;
const partInfo = (p: MidiPart) => {
  const program = p.drums ? 'GM drums' : p.program >= 0 ? GM_PROGRAMS[p.program] : '';
  return [program === p.name ? '' : program, plural(p.notes.length, 'note'), range(p.notes)].filter(Boolean).join(' · ');
};

function SongSummary(p: { file: string; song: MidiSong }) {
  const s = p.song;
  const ts = songTimeSig(s);
  const bars = Math.ceil(s.end / barTicksOf(ts.num, ts.den));
  return (
    <p class="hint sum">
      <b>{p.file}</b> — format {s.format}, {plural(s.parts.length, 'part')}, {plural(bars, 'bar')}, {fmtBpm(s.tempos[0].bpm)} BPM
      {s.tempos.length > 1 ? ` (${plural(s.tempos.length - 1, 'tempo change')})` : ''}, {ts.num}/{ts.den}
    </p>
  );
}

/** what the file has that the import leaves out */
function leftOut(s: MidiSong, extra: string[] = []) {
  const ts = songTimeSig(s);
  const out = [...extra];
  if (s.tempos.some((x) => x.bpm < 20 || x.bpm > 300)) out.push('tempos outside 20–300 BPM (retained; playback uses that range)');
  if (s.ignored.pitchBend) out.push(`pitch bend (${s.ignored.pitchBend} events)`);
  if (s.ignored.controllers) out.push(`other controllers (${s.ignored.controllers})`);
  if (s.ignored.aftertouch) out.push(`aftertouch (${s.ignored.aftertouch})`);
  // Program changes are retained and delivered; fixed synth presets keep their chosen sound.
  if (ts.changes) out.push(`${ts.changes} time signature change${ts.changes > 1 ? 's' : ''} (the project keeps ${ts.num}/${ts.den})`);
  if (!ts.supported) out.push(`the time signature ${s.timeSigs[0].num}/${s.timeSigs[0].den} (the project uses 4/4)`);
  return out;
}

function remapDrums(ns: MidiNote[]) {
  const out: MidiNote[] = [];
  let dropped = 0;
  for (const n of ns) {
    const k = drumLabKey(n.key);
    if (k === null) dropped++;
    else out.push(k === n.key ? n : { ...n, key: k });
  }
  return { notes: out, dropped };
}

// ---- as a new project ------------------------------------------------------------------------------
type Sf2Preset = { name: string; bank: number; program: number };
interface Choice {
  plugin: string;
  preset?: string;
  font?: string;
  bank?: number;
  program?: number;
  label: string;
  voices: number; // the part's most simultaneous notes, within what the plugin allows (SoundFont: sample voices)
  fx: Effect[]; // on the part's mixer insert (automatic choices only)
  lightened?: boolean; // a lighter instrument than the automatic one, or fewer SoundFont voices, to fit the CPU
}

const polyphonyParam = (plugins: PluginInfo[], id: string) => plugins.find((x) => x.id === id)?.params.find((x) => x.key === 'polyphony');
function voicesFor(plugins: PluginInfo[], id: string, need: number) {
  const pp = polyphonyParam(plugins, id);
  if (pp) return Math.max(pp.min, Math.min(pp.max, need));
  return Math.max(1, Math.min(plugins.find((x) => x.id === id)?.maxVoices ?? need, need));
}
const presetLabel = (plugins: PluginInfo[], id: string, preset?: string) => `${plugins.find((x) => x.id === id)?.name ?? id}${preset ? ` · ${preset}` : ''}`;

/** a part's instrument: `auto` (GM program -> built-in synth, or the chosen SoundFont), a plugin
 *  preset (`p:<id>|<preset>`) or a SoundFont from the library (`sf:<file>`) */
function resolve(part: MidiPart, choice: string, gmFont: string, fontPresets: Sf2Preset[] | null, plugins: PluginInfo[]): Omit<Choice, 'voices' | 'fx'> {
  const has = (id: string) => plugins.some((x) => x.id === id && x.type === 'instrument');
  const font = choice.startsWith('sf:') ? choice.slice(3) : choice === 'auto' ? gmFont : '';
  if (font && has(SOUNDFONT)) {
    const want = part.drums ? { bank: 128, program: Math.max(0, part.program) } : { bank: part.bank, program: Math.max(0, part.program) };
    let pick = want;
    if (fontPresets?.length) {
      // the exact GM preset, else the same program in bank 0 (or the standard kit), else the first
      const find = (b: number, p: number) => fontPresets.find((x) => x.bank === b && x.program === p);
      const hit = find(want.bank, want.program) ?? (part.drums ? find(128, 0) : find(0, want.program)) ?? (part.drums ? undefined : fontPresets[0]);
      if (hit) pick = { bank: hit.bank, program: hit.program };
      else if (choice === 'auto') return resolve(part, 'auto', '', null, plugins); // a font without drum kits: DrumLab
    }
    const name = fontPresets?.find((x) => x.bank === pick.bank && x.program === pick.program)?.name;
    return { plugin: SOUNDFONT, font, ...pick, label: `SoundFont ${pick.bank}:${pick.program}${name ? ` ${name}` : ''}` };
  }
  let inst = choice.startsWith('p:') ? { plugin: choice.slice(2).split('|')[0], preset: choice.split('|')[1] || undefined } : builtinInstrument(part.program, part.drums);
  if (!has(inst.plugin)) inst = { plugin: plugins.find((x) => x.type === 'instrument' && !x.resourceSlots?.length)?.id ?? inst.plugin, preset: undefined };
  const info = plugins.find((x) => x.id === inst.plugin);
  const preset = inst.preset && info?.presets.some((x) => x.name === inst.preset) ? inst.preset : undefined;
  return { plugin: inst.plugin, preset, label: presetLabel(plugins, inst.plugin, preset) };
}

// parts to channels: one each, or merged as a GM synth would play them (more than 16 parts: merged
// by default; a format 1 file often has a track per drum or per doubled guitar)
function ProjectImport(p: { file: string; song: MidiSong }) {
  const counts = useMemo(
    () => ({ part: p.song.parts.length, channel: mergeParts(p.song.parts, 'channel').length, instrument: mergeParts(p.song.parts, 'instrument').length }),
    [p.song],
  );
  const [merge, setMerge] = useState<MergeMode>(() => (counts.part <= 16 ? 'part' : counts.channel <= 16 ? 'channel' : 'instrument'));
  const song = useMemo(() => ({ ...p.song, parts: mergeParts(p.song.parts, merge) }), [p.song, merge]);
  return <ProjectImportDialog key={merge} file={p.file} song={song} original={p.song} merge={merge} setMerge={setMerge} counts={counts} />;
}

function ProjectImportDialog(p: {
  file: string;
  song: MidiSong;
  original: MidiSong;
  merge: MergeMode;
  setMerge: (m: MergeMode) => void;
  counts: Record<MergeMode, number>;
}) {
  const s = p.song;
  const plugins = useStore(catalog, (c) => c.plugins);
  const fonts = useStore(samples, (x) => x.library.list.filter((f) => f.type === 'sf2').map((f) => f.name).join('\n'))
    .split('\n')
    .filter(Boolean);
  const slotBytes = useStore(conn, (c) => c.server?.limits?.projectBytes ?? 0);
  const deflated = useStore(conn, (c) => (c.server?.limits?.projectDocBytes ?? 0) > 0); // slots hold the document deflated
  const [use, setUse] = useState(() => s.parts.map((_, i) => i < LIMITS.channels));
  const [inst, setInst] = useState(() => s.parts.map(() => 'auto'));
  const [gmFont, setGmFont] = useState('');
  const [fontPresets, setFontPresets] = useState<Record<string, Sf2Preset[]>>({});
  const [bars, setBars] = useState(4);
  const [withTempo, setWithTempo] = useState(true);
  const [withMarkers, setWithMarkers] = useState(true);
  const [busy, setBusy] = useState('');
  const [failed, setFailed] = useState('');

  // the presets of every font in use (for GM bank / program matching and the labels)
  const usedFonts = [...new Set([gmFont, ...inst.filter((x) => x.startsWith('sf:')).map((x) => x.slice(3))].filter(Boolean))];
  useEffect(() => {
    for (const f of usedFonts)
      if (!fontPresets[f])
        req('samples.presets', { name: f })
          .then((a) => setFontPresets((m) => ({ ...m, [f]: a.t === 'ack' ? ((a.presets as Sf2Preset[]) ?? []) : [] })))
          .catch(() => setFontPresets((m) => ({ ...m, [f]: [] })));
  }, [usedFonts.join('\n')]);

  const ts = songTimeSig(s);
  const bar = barTicksOf(ts.num, ts.den);
  const chosen = useMemo(() => s.parts.map((part, i) => ({ part, i })).filter((x) => use[x.i]).map((x, k) => ({
    ...x, part: canonicalMidiOrder(k ? x.part : { ...x.part,
      events: [...(x.part.events ?? []), ...(s.metadata ?? [])].sort((a,b) => a.tick-b.tick || a.order-b.order) }),
  })), [s, use]);
  // instruments: each part's choice, with as many voices as its notes need; automatic choices get
  // lighter when the estimated load would not fit the device
  const needs = useMemo(() => s.parts.map((x) => voicesNeeded(pedalNotes(x, ''))), [s]);
  const choices: Choice[] = chosen.map(({ part, i }) => {
    const font = inst[i].startsWith('sf:') ? inst[i].slice(3) : inst[i] === 'auto' ? gmFont : '';
    const c = resolve(part, inst[i], gmFont, font ? fontPresets[font] ?? null : null, plugins);
    // automatic built-in instruments get the effects that bring them closer to the GM original
    const fx = inst[i] === 'auto' && !c.font ? builtinEffects(part.program, part.drums).filter((e) => plugins.some((x) => x.id === e.plugin)) : [];
    return { ...c, voices: c.plugin === SOUNDFONT ? sf2Voices(needs[i], part.drums) : voicesFor(plugins, c.plugin, needs[i]), fx };
  });
  const fit = choices.map((c, k) => ({
    plugin: c.plugin,
    preset: c.preset,
    voices: c.voices,
    minVoices: c.plugin === SOUNDFONT ? sf2MinVoices(needs[chosen[k].i]) : undefined,
    auto: inst[chosen[k].i] === 'auto' && !c.font,
    fx: c.fx.map((e) => e.plugin),
  }));
  for (const k of fitLoad(fit, (id) => plugins.some((x) => x.id === id))) {
    const c = choices[k];
    const v = fit[k].plugin === SOUNDFONT ? fit[k].voices : voicesFor(plugins, fit[k].plugin, needs[chosen[k].i]);
    const label = fit[k].plugin === c.plugin && fit[k].preset === c.preset ? c.label : presetLabel(plugins, fit[k].plugin, fit[k].preset);
    choices[k] = { ...c, plugin: fit[k].plugin, preset: fit[k].preset, label, voices: v, fx: fit[k].fx.length ? c.fx : [], lightened: true };
  }
  const load = estimateLoad(choices.map((c) => ({ plugin: c.plugin, voices: c.voices, fx: c.fx.map((e) => e.plugin) })));
  const fxLabel = (c: Choice) => (c.fx.length ? ` + ${c.fx.map((e) => e.label).join(', ')}` : '');
  const key = `${use.join()}|${JSON.stringify(choices)}|${bars}|${withTempo}`;
  const plan = useMemo(() => {
    let dropped = 0;
    const parts = chosen.map(({ part }, k) => {
      if (!(part.drums && choices[k].plugin === DRUMLAB)) return { name: part.name, notes: pedalNotes(part, choices[k].plugin) };
      const r = remapDrums(pedalNotes(part, choices[k].plugin));
      dropped += r.dropped;
      return { name: part.name, notes: r.notes };
    });
    try {
      const controllerRows = chosen.filter(({ part }) => part.events?.length).length;
      const a = arrange(parts, bar, bars, LIMITS.patterns - 1 - controllerRows);
      for (const { part } of chosen) {
        if ((part.events ?? []).some((e) => JSON.stringify([e.tick, e.order, ...e.data]).length > 160000)) throw new Error(`${part.name}: one MIDI message exceeds the protocol frame capacity`);
        if ((part.events?.length ?? 0) > 65536 || (part.events ?? []).reduce((n, e) => n + e.data.length, 0) > 512 * 1024)
          throw new Error(`${part.name}: MIDI events exceed the device's lane capacity`);
      }
      const end = Math.max(a.end, s.end);
      if (end > LIMITS.clipTicks) throw new Error('the MIDI timeline is longer than the device playlist');
      const autos = withTempo ? tempoAutomation(s.tempos, end) : [];
      if (autos.length > 64) throw new Error('the tempo map needs more than 64 automation clips');
      if (a.clips.length + controllerRows + autos.length > LIMITS.clips) throw new Error('the notes and MIDI lanes exceed the playlist clip capacity');
      const fonts = new Map<string, Set<string>>();
      choices.forEach((choice, k) => {
        if (!choice.font) return;
        const selected = fonts.get(choice.font) ?? new Set<string>();
        for (const p of midiPresets(chosen[k].part)) selected.add(`${p.bank}:${p.program}`);
        selected.add(`${choice.bank ?? 0}:${choice.program ?? 0}`); fonts.set(choice.font, selected);
      });
      if ([...fonts.values()].some((x) => x.size > 1024)) throw new Error('the MIDI program plan exceeds the SoundFont preset capacity');
      return { a, autos, dropped };
    } catch (e) {
      return { err: (e as Error).message, dropped };
    }
  }, [key]);

  const instOptions = plugins.filter((x) => x.type === 'instrument' && x.id !== SOUNDFONT && !x.resourceSlots?.some((r) => r.audio));
  const controllerCount = chosen.filter(({ part }) => part.events?.length).length;
  const midiBytes = controllerCount * 300 + chosen.reduce((n, { part }) => n + JSON.stringify(midiRows(part.events ?? [])).length, 0);
  const bytes = plan.a ? estimateProjectBytes(plan.a, chosen.length, plan.autos!, withMarkers ? s.markers.length : 0) + midiBytes : 0;
  const stored = deflated ? estimateStoredBytes(bytes) : bytes;
  const tooMany = chosen.length > LIMITS.channels;
  const notes: string[] = [];
  if (plan.a && plan.a.segmentBars > bars) notes.push(`Patterns are ${plan.a.segmentBars} bars long: shorter ones would exceed the device's ${LIMITS.patterns} patterns or ${LIMITS.clips} clips.`);
  if (plan.dropped) notes.push(`${plan.dropped} drum notes have no DrumLab sound and are left out (choose a SoundFont for the drums to keep them).`);
  if (plan.a?.truncated) notes.push(`${plan.a.truncated} notes held over more than 64 bars are shortened.`);
  if (tooMany) notes.push(`${chosen.length} parts chosen; a project holds ${LIMITS.channels} channels: leave some out.`);
  if (chosen.length > 16) notes.push('More than 16 channels: the ones after the 16th share the master mixer track.');
  if (choices.some((c) => c.lightened && c.plugin !== SOUNDFONT)) notes.push('Some automatic instruments are lighter ones, so that the song fits the device\'s CPU (you can choose others).');
  if (choices.some((c) => c.lightened && c.plugin === SOUNDFONT)) notes.push('Some SoundFont parts get fewer voices, so that the song fits the device\'s CPU: long releases end sooner.');
  const big = slotBytes > 0 && stored > slotBytes;
  const out = leftOut(s);
  const sysCount = new Set(chosen.flatMap(({ part }) => (part.events ?? []).filter((e) => e.data[0] >= 0xf0 && e.data[0] !== 0xff).map((e) => JSON.stringify([e.tick, e.order, e.data])))).size;
  const unmapped = unmappedMidi(chosen.map((x) => x.part), choices.map((x) => x.plugin));
  if (unmapped.cc.length) notes.push(`Preserved without a built-in sound mapping: ${unmapped.cc.map((cc) => `CC ${cc}`).join(', ')}.`);
  if (unmapped.nrpn) notes.push('NRPN messages are preserved; their sound mapping is manufacturer-specific.');
  if (sysCount) notes.push(`${sysCount} SysEx/system messages are preserved. GM/GS/XG global reset, volume and tuning are applied; other vendor commands require a matching synthesizer.`);
  if (chosen.some(({ part }, k) => choices[k].plugin !== SOUNDFONT && part.events?.some((e) => [0xa0,0xd0].includes(e.data[0] & 0xf0))))
    notes.push('Aftertouch is preserved; choose SoundFont for pressure vibrato.');
  if (chosen.some(({ part }, k) => part.programChanges && choices[k].plugin !== SOUNDFONT))
    notes.push('Program changes are preserved. Choose a SoundFont to switch GM instruments during playback; a fixed synth preset keeps its selected sound.');

  async function run() {
    if (!plan.a) return;
    if (!confirm('The MIDI file replaces the current project. Unsaved changes are lost. Continue?')) return;
    setFailed('');
    try {
      await buildProject(s, p.file, chosen.map((x) => x.part), choices, plan.a, plan.autos!, withMarkers ? s.markers : [], setBusy);
      close();
      toast(`${p.file}: ${chosen.length} channels, ${plan.a.patterns.length + controllerCount} patterns, ${plan.a.clips.length + controllerCount} clips. Press play: the song plays in Song mode.`);
    } catch (e) {
      setBusy('');
      setFailed(`${(e as Error).message}. The project holds what was imported up to there.`);
    }
  }

  return (
    <Modal title="Import a MIDI file as a project" onClose={() => !busy && close()} wide>
      <div class="midi-imp">
        <SongSummary file={p.file} song={p.original} />
        {p.counts.instrument < p.counts.part ? (
          <div class="field">
            <label>Channels</label>
            <select value={p.merge} aria-label="Channels" onChange={(e) => p.setMerge((e.target as HTMLSelectElement).value as MergeMode)}>
              <option value="part">One per part ({p.counts.part})</option>
              {p.counts.channel < p.counts.part ? <option value="channel">One per MIDI channel and instrument ({p.counts.channel}), as a GM synth plays the file</option> : null}
              <option value="instrument">One per instrument ({p.counts.instrument})</option>
            </select>
          </div>
        ) : null}
        {fonts.length ? (
          <div class="field">
            <label>Automatic instruments</label>
            <select value={gmFont} aria-label="Automatic instruments" onChange={(e) => setGmFont((e.target as HTMLSelectElement).value)}>
              <option value="">Built-in synths (closest to each part's GM program)</option>
              {fonts.map((f) => (
                <option key={f} value={f}>
                  SoundFont {f} (each part's GM bank and program)
                </option>
              ))}
            </select>
          </div>
        ) : null}
        <div class="parts">
          <table class="grid">
            <thead>
              <tr>
                <th />
                <th>Part</th>
                <th>Instrument</th>
              </tr>
            </thead>
            <tbody>
              {s.parts.map((part, i) => {
                const k = chosen.findIndex((x) => x.i === i);
                return (
                  <tr key={i}>
                    <td>
                      <input
                        type="checkbox"
                        aria-label={`Import ${part.name}`}
                        checked={use[i]}
                        onChange={(e) => setUse(use.map((v, j) => (j === i ? (e.target as HTMLInputElement).checked : v)))}
                      />
                    </td>
                    <td>
                      <div>{part.name}</div>
                      <div class="hint">{partInfo(part)}</div>
                    </td>
                    <td>
                      <select
                        class="mini"
                        aria-label={`Instrument for ${part.name}`}
                        value={inst[i]}
                        disabled={!use[i]}
                        onChange={(e) => setInst(inst.map((v, j) => (j === i ? (e.target as HTMLSelectElement).value : v)))}
                      >
                        <option value="auto">Auto: {k >= 0 ? choices[k].label + fxLabel(choices[k]) : resolve(part, 'auto', gmFont, null, plugins).label}</option>
                        {fonts.map((f) => (
                          <option key={f} value={`sf:${f}`}>
                            SoundFont {f}
                          </option>
                        ))}
                        {instOptions.map((pl) => (
                          <optgroup key={pl.id} label={pl.name}>
                            {(pl.presets.length ? pl.presets : [{ name: '' }]).map((pr) => (
                              <option key={pr.name} value={`p:${pl.id}|${pr.name}`}>
                                {pl.name} · {pr.name || 'default'}
                              </option>
                            ))}
                          </optgroup>
                        ))}
                      </select>
                      {k >= 0 ? (
                        <div class="hint">
                          {plural(choices[k].voices, 'voice')} · ~{Math.round(choices[k].fx.reduce((a, e) => a + partCost(e.plugin, 0), partCost(choices[k].plugin, choices[k].voices)))} %
                          {choices[k].lightened ? (choices[k].plugin === SOUNDFONT ? ' · fewer voices, to fit the CPU' : ' · lighter, to fit the CPU') : ''}
                        </div>
                      ) : null}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
        <div class="row">
          <label>Patterns of</label>
          <select class="mini" aria-label="Pattern size" value={bars} onChange={(e) => setBars(+(e.target as HTMLSelectElement).value)}>
            {[1, 2, 4, 8, 16].map((b) => (
              <option key={b} value={b}>
                {b} bar{b > 1 ? 's' : ''}
              </option>
            ))}
          </select>
          <span class="hint">repeated parts share one pattern</span>
        </div>
        {s.tempos.length > 1 ? (
          <label class="check">
            <input type="checkbox" checked={withTempo} onChange={(e) => setWithTempo((e.target as HTMLInputElement).checked)} /> {plural(s.tempos.length - 1, 'tempo change')} as tempo automation
          </label>
        ) : null}
        {s.markers.length ? (
          <label class="check">
            <input type="checkbox" checked={withMarkers} onChange={(e) => setWithMarkers((e.target as HTMLInputElement).checked)} /> {plural(s.markers.length, 'marker')} on the playlist
          </label>
        ) : null}
        {plan.a ? (
          <p class="sum" data-testid="midi-plan">
            → {chosen.length} channels, {plan.a.patterns.length + controllerCount} patterns, {plan.a.clips.length + controllerCount} clips{plan.autos!.length ? ', tempo automation' : ''}; about {Math.round(bytes / 1024)} KB
            {deflated ? ` (~${Math.round(stored / 1024)} KB compressed)` : ''}
            {slotBytes ? ` of the ${Math.round(slotBytes / 1024)} KB a project slot holds` : ''}
          </p>
        ) : (
          <p class="err">{plan.err}</p>
        )}
        {plan.a ? (
          <p class={`sum${load > LOAD_LIMIT ? ' warn' : ''}`} data-testid="midi-load">
            Estimated DSP load ~{Math.round(load)} % of the audio deadline
            {load > LOAD_LIMIT ? `: above ${LOAD_LIMIT} %, the device will skip notes it has no time for. Choose lighter instruments or fewer parts.` : ' (measured costs; an estimate).'}
          </p>
        ) : null}
        {big ? (
          <p class="warn">
            Larger than a project slot: it plays, but the device cannot save it (and the autosave keeps the previous project). Import fewer parts to save it.
          </p>
        ) : null}
        {notes.map((n) => (
          <p key={n} class="hint warn">
            {n}
          </p>
        ))}
        {out.length ? <p class="hint">Not applied: {out.join(', ')}.</p> : null}
        {s.warnings.map((w) => (
          <p key={w} class="hint warn">
            {w}
          </p>
        ))}
        {failed ? <p class="err">{failed}</p> : null}
        <div class="buttons">
          {busy ? <span class="hint grow">{busy}</span> : <span class="hint grow">Replaces the current project (save it first to keep it).</span>}
          <Btn disabled={!!busy} onClick={close}>Cancel</Btn>
          <Btn active disabled={!!busy || !plan.a || !chosen.length || tooMany} onClick={run}>
            Import
          </Btn>
        </div>
      </div>
    </Modal>
  );
}

/** an operation that must succeed (req resolves errors too: they carry t = 'err') */
async function must(t: string, args: Record<string, unknown> = {}) {
  const a = await req(t, args, 20000);
  if (a.t === 'err') throw new Error(`${t}: ${a.msg ?? a.code}`);
  return a;
}

async function buildProject(
  s: MidiSong,
  file: string,
  parts: MidiPart[],
  choices: Choice[],
  a: Arrangement,
  autos: AutoPlan[],
  markers: { tick: number; name: string }[],
  progress: (text: string) => void,
) {
  const tempoTrack = autos.length ? parts.length : -1;
  const tracksNeeded = parts.length + (autos.length ? 1 : 0);
  const total = 6 + parts.length * 3 + choices.filter((c) => c.font).length + choices.reduce((n, c) => n + c.fx.reduce((m, e) => m + 1 + Object.keys(e.params).length, 0), 0) + tracksNeeded * 2 + a.patterns.length * 2 + autos.length + markers.length + 3;
  let done = 0;
  const step = async (t: string, args: Record<string, unknown> = {}) => {
    progress(`Importing… ${Math.min(99, Math.round((done++ / total) * 100))} %`);
    return must(t, args);
  };
  const ts = songTimeSig(s);
  const title = /^(control track|untitled|track ?\d*|)$/i.test(s.title.trim()) ? file.replace(/\.[^.]+$/, '') : s.title;

  await step('stop');
  await step('project.new', { template: 'empty' });
  const initial = (P()?.patterns ?? []).map((x) => x.id);
  await step('project.rename', { name: title.slice(0, 64) });
  await step('bpm', { bpm: Math.min(300, Math.max(20, s.tempos[0].bpm)) });
  await step('timesig', { num: ts.num, den: ts.den });

  // channels (before the patterns: a new channel adds a row to every pattern)
  const chan: number[] = [];
  const colors: string[] = [];
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    const c = choices[i];
    const color = part.drums ? DRUM_COLOR : COLORS[i % COLORS.length];
    colors.push(color);
    const r = await step('channel.add', { plugin: c.plugin, name: part.name.slice(0, 64), color, ...(c.preset ? { preset: c.preset } : {}), ...(part.drums ? { icon: 'drum' } : {}) });
    const id = r.newId as number;
    chan.push(id);
    if (c.font) {
      const slot = catalog.state.plugins.find((x) => x.id === SOUNDFONT)?.resourceSlots?.[0]?.key ?? 'soundfont';
      const presets = choices.flatMap((x, k) => x.font === c.font ? [{ bank: x.bank ?? 0, program: x.program ?? 0 }, ...midiPresets(parts[k])] : []);
      const unique = [...new Map(presets.map((p) => [`${p.bank}:${p.program}`, p])).values()];
      await step('channel.resource', { channel: id, slot, sample: c.font, bank: c.bank ?? 0, program: c.program ?? 0, presets: unique });
    }
    // as many voices as the part plays at once (a preset may have set more): fewer voices cost less
    if (polyphonyParam(catalog.state.plugins, c.plugin)) await step('channel.param', { channel: id, key: 'polyphony', v: c.voices });
    // effects on its own mixer insert (not on the master, which every channel shares)
    const track = P()?.channels.find((x) => x.id === id)?.track ?? 0;
    for (const e of track > 0 ? c.fx : []) {
      const f = await step('fx.add', { track, plugin: e.plugin });
      for (const [key, v] of Object.entries(e.params)) await step('fx.param', { fx: f.newId as number, key, v });
    }
    await step('channel.set', { channel: id, vol: Math.min(1.25, Math.max(0, 0.78 * part.volume)), pan: part.pan });
  }

  // playlist tracks: one per part (and one for the tempo)
  for (let t = P()?.playlist.tracks.length ?? 0; t < tracksNeeded; t++) await step('ptrack.set', { add: true });
  for (let t = 0; t < tracksNeeded; t++)
    await step('ptrack.set', t === tempoTrack ? { index: t, name: 'Tempo', color: '#40c4aa' } : { index: t, name: parts[t].name.slice(0, 64), color: colors[t] });

  // patterns, each with its part's notes
  const patId: number[] = [];
  for (const pl of a.patterns) {
    const r = await step('pattern.add', { name: pl.name, color: colors[pl.part], select: false, length: pl.length });
    const id = r.newId as number;
    patId.push(id);
    await step('notes.set', { pattern: id, channel: chan[pl.part], notes: pl.notes.map((n, k) => noteRow(n, k + 1)) });
  }
  // Controller clips run once over the song: deduplicating notes must never repeat a bend,
  // reset, bank/program selection, or bulk dump at the start of a repeated note pattern.
  const midiClips: { kind: string; ref: number; track: number; start: number; len: number; offset: number }[] = [];
  for (let k = 0; k < parts.length; ++k) {
    const events = midiRows(parts[k].events ?? []);
    if (!events.length) continue;
    const length = Math.min(LIMITS.clipTicks, Math.max(a.end, s.end));
    const r = await step('pattern.add', { name: `${parts[k].name} MIDI`.slice(0, 64), select: false, length });
    const pattern = r.newId as number;
    let chunk: number[][] = [], size = 0, append = false;
    const flush = async () => {
      if (!chunk.length) return;
      await step('midi.set', { pattern, channel: chan[k], events: chunk, append });
      append = true; chunk = []; size = 0;
    };
    for (const e of events) {
      const n = JSON.stringify(e).length;
      if (size + n > 160000 || chunk.length >= 2048) await flush();
      if (n > 160000) throw new Error('one MIDI message exceeds the protocol frame capacity');
      chunk.push(e); size += n + 1;
    }
    await flush();
    midiClips.push({ kind: 'pattern', ref: pattern, track: k, start: 0, len: length, offset: 0 });
  }
  for (const id of initial) await step('pattern.del', { pattern: id });

  // tempo automation, then every clip at once
  const autoId: number[] = [];
  for (const x of autos) {
    const r = await step('auto.add', { name: 'Tempo', target: { kind: 'tempo' }, length: x.length, points: x.points });
    autoId.push(r.newId as number);
  }
  const clips = [
    ...midiClips,
    ...a.clips.map((c) => ({ kind: 'pattern', ref: patId[c.pattern], track: c.part, start: c.start, len: c.len, offset: 0 })),
    ...autos.map((x, k) => ({ kind: 'automation', ref: autoId[k], track: tempoTrack, start: x.start, len: x.length, offset: 0 })),
  ];
  await step('clips.set', { clips });
  for (const m of markers) await step('marker', { action: 'add', tick: m.tick, name: m.name });

  await step('mode', { mode: 'song' });
  if (patId.length) await step('pattern.select', { pattern: patId[0], queue: false });
  await step('seek', { tick: 0 });
  clearHistory();
  ui.set((u) => ({ selectedChannel: chan[0] ?? u.selectedChannel, pianoChannel: chan[0] ?? u.pianoChannel, panels: { ...u.panels, playlist: true }, rollFit: u.rollFit + 1 }));
}

// ---- into the piano roll ---------------------------------------------------------------------------
function PianoImport(p: { file: string; song: MidiSong; pattern: number; channel: number }) {
  const s = p.song;
  const ch = channelById(p.channel);
  const pat = patternById(p.pattern);
  const drumLab = ch?.plugin.id === DRUMLAB;
  // a single part, or the drums for DrumLab, or the first melodic part
  const [use, setUse] = useState(() => {
    const pick = s.parts.length === 1 ? 0 : Math.max(0, s.parts.findIndex((x) => x.drums === drumLab));
    return s.parts.map((_, i) => i === pick);
  });
  const ts = songTimeSig(s);
  const bar = barTicksOf(ts.num, ts.den);
  const totalBars = Math.max(1, Math.ceil(s.end / bar));
  const maxBars = Math.max(1, Math.floor(LIMITS.noteStartTicks / bar));
  const [from, setFrom] = useState(1);
  const [to, setTo] = useState(Math.min(totalBars, maxBars));
  const [mode, setMode] = useState<'replace' | 'add'>('replace');
  const [mapDrums, setMapDrums] = useState(true);
  const [setTempo, setSetTempo] = useState(false);
  const [fitLength, setFitLength] = useState(true);

  const sel = s.parts.filter((_, i) => use[i]);
  const independent = new Set(sel.filter((x) => x.events?.some((e) => e.data[0] < 0xf0)).map((x) => `${x.port ?? 0}:${x.channel}`)).size > 1;
  const remap = drumLab && mapDrums;
  let dropped = 0;
  const lists = sel.map((x) => {
    if (!(remap && x.drums)) return pedalNotes(x, ch?.plugin.id ?? '');
    const r = remapDrums(pedalNotes(x, ch?.plugin.id ?? ''));
    dropped += r.dropped;
    return r.notes;
  });
  const a = Math.max(1, Math.min(from, totalBars));
  const b = Math.max(a, Math.min(to, a + maxBars - 1));
  const slice = sliceNotes(lists, (a - 1) * bar, b * bar);
  const existing = mode === 'add' ? A.notesOf(p.pattern, p.channel).length : 0;
  const room = LIMITS.notesPerPattern - existing;
  const end = slice.notes.reduce((m, n) => Math.max(m, n.start + n.len), 0);
  const fixedShort = !!pat && pat.length > 0 && end > pat.length;

  function apply() {
    if (!ch || !pat || independent || !sel.length) return;
    const before = A.notesOf(p.pattern, p.channel);
    let id = mode === 'add' ? nextId(before) : 1;
    const fromTick = (a - 1) * bar, toTick = b * bar;
    const sliced = sliceMidi([...sel, { ...sel[0], notes: [], events: s.metadata ?? [] }], fromTick, toTick);
    const add = slice.notes.slice(0, Math.max(0, room)).map((n) => ({
      ...newNote(id++, n.start, n.len, n.key, n.vel), rel: n.rel ?? .5,
      midiOnOrder: (n.onOrder ?? 0) + sliced.shift, midiOffOrder: (n.offOrder ?? 0) + sliced.shift,
    }));
    const all = (mode === 'add' ? [...before, ...add] : add).sort((x, y) => x.start - y.start || x.key - y.key);
    const oldMidi = mode === 'add' ? pat.seqs.find((q) => q.channel === p.channel)?.midi ?? [] : [];
    A.setNotes(p.pattern, p.channel, all, 'Import MIDI', before, [...oldMidi, ...midiRows(sliced.events)]);
    if (fixedShort && fitLength) A.setPattern(p.pattern, { length: 0 });
    if (setTempo) A.setBpm(Math.min(300, Math.max(20, Math.round(s.tempos[0].bpm * 10) / 10)), true);
    ui.set((u) => ({ rollFit: u.rollFit + 1 }));
    toast(`${add.length} notes from ${p.file} in ${ch.name} (${pat.name})`);
    close();
  }

  const num = (v: string, f: (n: number) => void) => {
    const n = Math.round(+v);
    if (Number.isFinite(n)) f(n);
  };
  return (
    <Modal title={`Import MIDI into ${ch?.name ?? 'the channel'}`} onClose={close} wide>
      <div class="midi-imp">
        <SongSummary file={p.file} song={s} />
        <p class="hint sum">
          Into the piano roll of <b>{ch?.name}</b> in “{pat?.name}”.
        </p>
        <div class="parts">
          {s.parts.map((part, i) => (
            <label class="check" key={i}>
              <input type="checkbox" aria-label={`Import ${part.name}`} checked={use[i]} onChange={(e) => setUse(use.map((v, j) => (j === i ? (e.target as HTMLInputElement).checked : v)))} />
              <span>{part.name}</span>
              <span class="hint">{partInfo(part)}</span>
            </label>
          ))}
        </div>
        <div class="row">
          <label>Bars</label>
          <input type="number" aria-label="From bar" min={1} max={totalBars} value={from} style={{ width: '64px' }} onInput={(e) => num((e.target as HTMLInputElement).value, setFrom)} />
          <span>to</span>
          <input type="number" aria-label="To bar" min={1} max={totalBars} value={to} style={{ width: '64px' }} onInput={(e) => num((e.target as HTMLInputElement).value, setTo)} />
          <span class="hint">of {totalBars}</span>
        </div>
        <div class="row" style={{ marginTop: '6px' }}>
          <label class="check">
            <input type="radio" name="midi-mode" checked={mode === 'replace'} onChange={() => setMode('replace')} /> replace the notes
          </label>
          <label class="check">
            <input type="radio" name="midi-mode" checked={mode === 'add'} onChange={() => setMode('add')} /> add to the notes
          </label>
        </div>
        {drumLab && sel.some((x) => x.drums) ? (
          <label class="check">
            <input type="checkbox" checked={mapDrums} onChange={(e) => setMapDrums((e.target as HTMLInputElement).checked)} /> play GM drums on DrumLab's closest pieces
          </label>
        ) : null}
        <label class="check">
          <input type="checkbox" checked={setTempo} onChange={(e) => setSetTempo((e.target as HTMLInputElement).checked)} /> set the project tempo to the file's {fmtBpm(s.tempos[0].bpm)} BPM
        </label>
        {fixedShort ? (
          <label class="check">
            <input type="checkbox" checked={fitLength} onChange={(e) => setFitLength((e.target as HTMLInputElement).checked)} /> make the pattern's length automatic (it is fixed at{' '}
            {Math.round(pat!.length / bar)} bars; the notes need {Math.ceil(end / bar)})
          </label>
        ) : null}
        <p class="sum" data-testid="midi-plan">
          → {Math.min(slice.notes.length, Math.max(0, room))} notes over {b - a + 1} bars{mode === 'add' && existing ? `, added to ${existing}` : ''}
        </p>
        {slice.notes.length > room ? <p class="warn">A pattern row holds {LIMITS.notesPerPattern} notes: the last {slice.notes.length - room} are left out (import fewer bars).</p> : null}
        {independent ? <p class="warn">These MIDI channels have independent controller streams. Import them as a project, or choose one MIDI channel here, to keep their expression.</p> : null}
        {dropped ? <p class="hint warn">{dropped} drum notes have no DrumLab sound and are left out.</p> : null}
        {slice.clipped ? <p class="hint">{slice.clipped} notes that sound past bar {b} end there.</p> : null}
        {leftOut(s).length ? <p class="hint">Not applied: {leftOut(s).join(', ')}.</p> : null}
        <div class="buttons">
          <span class="grow" />
          <Btn onClick={close}>Cancel</Btn>
          <Btn active disabled={independent || !sel.length || !slice.notes.length || room <= 0} onClick={apply}>
            Import
          </Btn>
        </div>
      </div>
    </Modal>
  );
}
