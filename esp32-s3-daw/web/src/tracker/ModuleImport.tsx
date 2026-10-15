// SPDX-License-Identifier: GPL-3.0-or-later
// Tracker module import (docs/TRACKER.md). The file goes to the device's sample library (like a
// sample), the Tracker plugin reads it there (samples.probe: length, channels, song tempo), and the
// song becomes one long note on one or more Tracker channels, in one pattern with one clip:
//   new project   File > Import tracker module…, or a module dropped on the browser panel
//   add           the same dialog, into the current project on a new playlist track
// Playback is libxmp's: the note plays the song, the transport can start anywhere inside it (note
// chase), and the project tempo speeds it up or slows it down. Everything goes through the
// ordinary editing operations: no privileged endpoint.
import { render } from 'preact';
import { useEffect, useState } from 'preact/hooks';

import { clearHistory } from '../app/history';
import { req } from '../app/protocol';
import { catalog, live, P, samples, toast, ui } from '../app/state';
import { Btn, Modal } from '../ui/controls';
import {
  FORMAT_BY_INDEX,
  FORMAT_NAMES,
  fmtTime,
  importBytes,
  importPlan,
  libraryName,
  moduleCrc,
  MODULE_ACCEPT,
  PPQ,
  readModuleHeader,
  splitsFor,
  TRACKER,
} from './module';
import type { SampleLibrary } from '../app/types';
import type { ChannelGroup, ModuleHeader, ModuleInfo, Split } from './module';

const COLORS = ['#4cc9e0', '#5fb3ff', '#4cd9a0', '#ffb03a', '#9a7bff', '#ff7ad9', '#c6e04a', '#ff8a5c'];
const PSRAM_RESERVE = 2 << 20; // the device keeps this free for the project and the protocol

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
    inp.accept = MODULE_ACCEPT;
    inp.setAttribute('aria-label', 'Tracker module');
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

const hasTracker = () => catalog.state.plugins.some((p) => p.id === TRACKER);

/** a module from this computer (asks for one when not given): upload, read, import */
export async function importModule(file?: File) {
  const f = file ?? (await pickFile());
  if (!f) return;
  if (!hasTracker()) {
    toast('This firmware has no Tracker plugin', 'error');
    return;
  }
  const header = readModuleHeader(new Uint8Array(await f.slice(0, 4096).arrayBuffer()));
  if (!header) {
    toast(`${f.name}: not a MOD (M.K. / xCHN / xxCH), XM, S3M or IT module the Tracker plugin plays`, 'error');
    return;
  }
  mount(<ModuleImport file={f} header={header} />);
}

/** a module already in the device's sample library */
export function importLibraryModule(name: string) {
  if (!hasTracker()) {
    toast('This firmware has no Tracker plugin', 'error');
    return;
  }
  mount(<ModuleImport library={name} />);
}

/** POST to the sample library with upload progress (fetch has none) */
function upload(file: File, onProgress: (pct: number) => void): Promise<void> {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open('POST', `/api/samples?name=${encodeURIComponent(file.name)}&reclaim=modules`);
    x.upload.onprogress = (e) => e.lengthComputable && onProgress(Math.round((e.loaded / e.total) * 100));
    x.onload = () => {
      let j: { error?: string } = {};
      try {
        j = JSON.parse(x.responseText);
      } catch {
        /* not JSON */
      }
      if (x.status !== 200) reject(new Error(j.error ?? `${x.status} ${x.statusText}`));
      else {
        samples.set({ library: j as SampleLibrary });
        resolve();
      }
    };
    x.onerror = () => reject(new Error('the upload failed (connection)'));
    x.send(file);
  });
}

async function must(t: string, args: Record<string, unknown> = {}) {
  const a = await req(t, args, 20000);
  if (a.t === 'err') throw new Error(`${t}: ${a.msg ?? a.code}`);
  return a;
}

type Phase = { kind: 'upload'; pct: number } | { kind: 'probe' } | { kind: 'ready'; info: ModuleInfo } | { kind: 'error'; msg: string };

function ModuleImport(p: { file?: File; header?: ModuleHeader; library?: string }) {
  const name = p.file ? libraryName(p.file.name) : p.library!;
  const title = (p.header?.title || name.replace(/\.[^.]+$/, '')).slice(0, 48);
  const [phase, setPhase] = useState<Phase>(p.file ? { kind: 'upload', pct: 0 } : { kind: 'probe' });
  const [target, setTarget] = useState<'new' | 'add'>('new');
  const [split, setSplit] = useState<Split>('one');
  const [withTempo, setWithTempo] = useState(true);
  const [busy, setBusy] = useState('');
  const [failed, setFailed] = useState('');

  useEffect(() => {
    (async () => {
      try {
        if (p.file) {
          // Another client may have opened / removed a module since our last snapshot.
          const response = await fetch('/api/samples');
          if (!response.ok) throw new Error('Cannot read the sample library');
          const library = (await response.json()) as SampleLibrary;
          samples.set({ library });
          const have = library.list.find((x) => x.name === name);
          const crc = moduleCrc(new Uint8Array(await p.file.arrayBuffer()));
          if (!have || have.size !== p.file.size || have.crc !== crc)
            await upload(p.file, (pct) => setPhase({ kind: 'upload', pct }));
        }
        // Cached / library opens also clear previous songs, without re-uploading this one.
        const kept = await must('samples.keepModule', { name });
        if (kept.library) samples.set({ library: kept.library as SampleLibrary });
        setPhase({ kind: 'probe' });
        const a = await req('samples.probe', { name, plugin: TRACKER }, 60000);
        if (a.t === 'err') throw new Error(String(a.msg ?? a.code));
        setPhase({ kind: 'ready', info: a.values as unknown as ModuleInfo });
      } catch (e) {
        setPhase({ kind: 'error', msg: (e as Error).message });
      }
    })();
  }, []);

  const info = phase.kind === 'ready' ? phase.info : null;
  const pr = P();
  const ts = target === 'add' && pr ? pr.meta.timeSig : ([4, 4] as [number, number]);
  const barTicks = ((PPQ * 4) / ts[1]) * ts[0];
  const plan = info ? importPlan(info, split, title, barTicks) : null;
  const songTempo = plan?.songTempo ?? 125;
  const span = plan?.span ?? null;
  const groups: ChannelGroup[] = plan?.groups ?? [];
  const fileBytes = samples.state.library.list.find((x) => x.name === name)?.size ?? p.file?.size ?? 0;
  const need = info ? importBytes(fileBytes, info.memory, groups.length) : 0;
  const d = live.state.diag;
  const psramTotal = Number(d?.psramTotal ?? 0), psramFree = Number(d?.psramFree ?? 0);
  // free PSRAM next to the current project (a new project frees its channels first)
  const free = psramTotal ? psramFree - PSRAM_RESERVE : Infinity;
  const tooBig = !!info && target === 'add' && need > free;
  const format = info ? FORMAT_BY_INDEX[info.format] : p.header?.format;

  async function run() {
    if (!info || !span) return;
    if (target === 'new' && !confirm('The module replaces the current project. Unsaved changes are lost. Continue?')) return;
    setFailed('');
    try {
      await build({ name, title, songTempo, groups, target, withTempo: target === 'new' || withTempo, span, progress: setBusy });
      close();
      toast(`${title}: ${groups.length} Tracker channel${groups.length > 1 ? 's' : ''}, ${fmtTime(info.length)}. Press play: the song plays in Song mode.`);
    } catch (e) {
      setBusy('');
      setFailed(`${(e as Error).message}. The project holds what was imported up to there.`);
    }
  }

  return (
    <Modal title="Import a tracker module" onClose={() => !busy && close()} wide>
      <div class="midi-imp" data-testid="module-import">
        <p class="hint">Only the module you open stays on the device. Other MOD, XM, S3M and IT files are cleared, including files used by saved projects; reimport them when needed. SoundFonts and WAVs are kept.</p>
        <p class="sum">
          <b>{title}</b>
          {format ? ` · ${FORMAT_NAMES[format]}` : ''} · {name}
          {fileBytes ? ` · ${fileBytes >= 1048576 ? `${(fileBytes / 1048576).toFixed(1)} MB` : `${Math.round(fileBytes / 1024)} KB`}` : ''}
        </p>
        {phase.kind === 'upload' ? <p class="hint">Uploading to the device's sample library… {phase.pct} %</p> : null}
        {phase.kind === 'probe' ? <p class="hint">The device reads the module…</p> : null}
        {phase.kind === 'error' ? <p class="err">{phase.msg}</p> : null}
        {info ? (
          <>
            <p class="sum" data-testid="module-info">
              {fmtTime(info.length)} · {info.channels} channel{info.channels === 1 ? '' : 's'} · {info.orders} order{info.orders === 1 ? '' : 's'} · {info.instruments} instrument{info.instruments === 1 ? '' : 's'} ·
              song tempo {songTempo} BPM · player memory ~{info.memory} KB per channel
            </p>
            <div class="field">
              <label>Import</label>
              <select value={target} aria-label="Import into" onChange={(e) => setTarget((e.target as HTMLSelectElement).value as 'new' | 'add')}>
                <option value="new">As a new project (replaces the current one)</option>
                <option value="add">Into the current project, on a new playlist track</option>
              </select>
            </div>
            <div class="field">
              <label>Channels</label>
              <select value={String(split)} aria-label="Channels" onChange={(e) => {
                const v = (e.target as HTMLSelectElement).value;
                setSplit(v === 'one' || v === 'each' ? v : (+v as 4 | 8));
              }}>
                {splitsFor(info.channels).map((s) => (
                  <option key={String(s)} value={String(s)}>
                    {s === 'one'
                      ? 'The whole song on one channel'
                      : s === 'each'
                        ? `One channel per tracker channel (${info.channels}): its own mixer track, effects and mute each`
                        : `Groups of ${s} tracker channels (${Math.ceil(info.channels / s)} channels)`}
                  </option>
                ))}
              </select>
              {groups.length > 1 ? (
                <span class="hint warn" data-testid="module-split-warning">
                  Every channel runs its own copy of the player (memory and CPU for each), and their ticks start in the same audio block. Measured on
                  the board: a 4-channel MOD on 4 channels took 29 % CPU and was late for 55 blocks a minute (on one channel: 12 %, never late). Split
                  a song only to give its parts their own effects or mixer tracks.
                </span>
              ) : null}
            </div>
            {target === 'add' ? (
              <label class="check">
                <input type="checkbox" checked={withTempo} onChange={(e) => setWithTempo((e.target as HTMLInputElement).checked)} /> Set the project tempo to the song's (
                {songTempo} BPM); otherwise the song plays faster or slower with the project tempo
              </label>
            ) : null}
            {span ? (
              <p class="sum" data-testid="module-plan">
                → {groups.length} Tracker channel{groups.length > 1 ? 's' : ''}, one pattern of {Math.round(span.pattern / barTicks)} bars, one clip; about{' '}
                {Math.round(need / 1024)} KB of PSRAM{Number.isFinite(free) ? ` (${Math.round(Math.max(0, free) / 1024)} KB free now)` : ''}
              </p>
            ) : (
              <p class="err">The song is longer than 1000 bars at its tempo: it does not fit in one pattern.</p>
            )}
            {tooBig ? <p class="warn">Not enough free PSRAM next to the current project: import it as a new project or with fewer channels.</p> : null}
          </>
        ) : null}
        {failed ? <p class="err">{failed}</p> : null}
        <div class="buttons">
          {busy ? <span class="hint grow">{busy}</span> : <span class="hint grow">{target === 'new' ? 'Replaces the current project (save it first to keep it).' : ''}</span>}
          <Btn disabled={!!busy} onClick={close}>
            Cancel
          </Btn>
          <Btn active disabled={!!busy || !info || !span || tooBig} onClick={run}>
            Import
          </Btn>
        </div>
      </div>
    </Modal>
  );
}

async function build(o: {
  name: string;
  title: string;
  songTempo: number;
  groups: ChannelGroup[];
  target: 'new' | 'add';
  withTempo: boolean;
  span: { note: number; pattern: number };
  progress: (t: string) => void;
}) {
  const total = 8 + o.groups.length * 5;
  let done = 0;
  const step = async (t: string, args: Record<string, unknown> = {}) => {
    o.progress(`Importing… ${Math.min(99, Math.round((done++ / total) * 100))} %`);
    return must(t, args);
  };
  const slot = catalog.state.plugins.find((x) => x.id === TRACKER)?.resourceSlots?.[0]?.key ?? 'module';
  let patId: number;
  let track: number;
  if (o.target === 'new') {
    await step('stop');
    await step('project.new', { template: 'empty' });
    await step('project.rename', { name: o.title.slice(0, 64) });
    await step('timesig', { num: 4, den: 4 });
    patId = P()!.patterns[0].id;
    if (!P()!.playlist.tracks.length) await step('ptrack.set', { add: true });
    await step('pattern.set', { pattern: patId, name: o.title.slice(0, 64), color: COLORS[0], length: o.span.pattern });
    track = 0;
  } else {
    const a = await step('pattern.add', { name: o.title.slice(0, 64), color: COLORS[0], select: false, length: o.span.pattern });
    patId = a.newId as number;
    track = P()?.playlist.tracks.length ?? 0;
    await step('ptrack.set', { add: true });
  }
  if (o.withTempo) await step('bpm', { bpm: o.songTempo });
  await step('ptrack.set', { index: track, name: o.title.slice(0, 64), color: COLORS[0] });
  const ids: number[] = [];
  for (let i = 0; i < o.groups.length; i++) {
    const g = o.groups[i];
    const r = await step('channel.add', { plugin: TRACKER, name: g.name.slice(0, 64), color: COLORS[i % COLORS.length] });
    const id = r.newId as number;
    ids.push(id);
    await step('channel.resource', { channel: id, slot, sample: o.name });
    await step('channel.param', { channel: id, key: 'tempo', v: o.songTempo });
    if (g.first !== 1 || g.last !== 64) {
      await step('channel.param', { channel: id, key: 'first', v: g.first });
      await step('channel.param', { channel: id, key: 'last', v: g.last });
    }
    await step('notes.set', { pattern: patId, channel: id, notes: [[1, 0, o.span.note, 60, 0.78]] });
  }
  if (o.target === 'new') await step('clips.set', { clips: [{ kind: 'pattern', ref: patId, track, start: 0, len: o.span.pattern, offset: 0 }] });
  else await step('clip.add', { kind: 'pattern', ref: patId, track, start: 0, len: o.span.pattern, offset: 0 });
  await step('mode', { mode: 'song' });
  if (o.target === 'new') {
    await step('seek', { tick: 0 });
    clearHistory();
  }
  ui.set((u) => ({ selectedChannel: ids[0] ?? u.selectedChannel, panels: { ...u.panels, playlist: true } }));
}
