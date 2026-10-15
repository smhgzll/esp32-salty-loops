// SPDX-License-Identifier: GPL-3.0-or-later
// Channel rack / step sequencer. One row per channel: mute, pan, volume, mixer track, name (opens
// the instrument), the step row of the current pattern (or a preview of its piano-roll notes), the
// row's own step count. Steps are painted by dragging; each gesture is one undo step.
import { useRef, useState } from 'preact/hooks';

import * as A from '../app/actions';
import { catalog, live, proj, toast, ui } from '../app/state';
import { useStore } from '../app/store';
import type { Channel, Pattern, StepArr } from '../app/types';
import { toNote } from '../app/types';
import { PALETTE } from '../app/util';
import { Btn, DragNumber, Icon, Knob, Led, menuAt, showMenu } from '../ui/controls';
import type { MenuItem } from '../ui/controls';
import { PatternSelector, togglePanel } from '../transport/TopBar';

const STEP_TICKS = 240;

function longPress(handler: (e: PointerEvent) => void) {
  let t: ReturnType<typeof setTimeout> | null = null;
  return {
    onPointerDown: (e: PointerEvent) => {
      if (e.pointerType !== 'touch') return;
      t = setTimeout(() => {
        t = null;
        handler(e);
      }, 500);
    },
    onPointerUp: () => t && clearTimeout(t),
    onPointerCancel: () => t && clearTimeout(t),
    onPointerLeave: () => t && clearTimeout(t),
  };
}

export function instrumentMenu(onPick: (id: string, preset?: string) => void): MenuItem[] {
  return catalog.state.plugins
    .filter((p) => p.type === 'instrument')
    .map((p) => ({ label: `${p.name}  ·  ${p.category}`, action: () => onPick(p.id) }));
}

export function channelMenu(c: Channel, pat: Pattern | undefined): MenuItem[] {
  const seq = A.seqOf(pat, c.id);
  const patId = pat?.id ?? 0;
  return [
    { label: 'Open instrument', action: () => openEditor(c.id) },
    { label: 'Piano roll', action: () => openPiano(c.id) },
    { label: seq?.pianoRoll ? 'Use step sequencer' : 'Convert steps to piano roll', action: () => A.setSeqMode(patId, c.id, !seq?.pianoRoll) },
    { sep: true, label: '' },
    { label: 'Rename…', action: () => { const n = prompt('Channel name', c.name); if (n) A.setChannel(c.id, { name: n }); } },
    { label: 'Color…', action: () => showMenu(PALETTE.map((col) => ({ label: col, action: () => A.setChannel(c.id, { color: col }) })), 200, 120) },
    { label: 'Mixer track…', action: () => showMenu(Array.from({ length: 17 }, (_, i) => ({ label: i ? `Insert ${i}` : 'Master', checked: c.track === i, action: () => A.setChannel(c.id, { track: i }) })), 200, 60) },
    { label: 'Replace instrument…', action: () => showMenu(instrumentMenu((id) => A.setChannelPlugin(c.id, id)), 220, 120) },
    { sep: true, label: '' },
    { label: 'Fill every 2 steps', action: () => A.stepOp(patId, c.id, 'fill2') },
    { label: 'Fill every 4 steps', action: () => A.stepOp(patId, c.id, 'fill4') },
    { label: 'Fill every 8 steps', action: () => A.stepOp(patId, c.id, 'fill8') },
    { label: 'Shift left', action: () => A.stepOp(patId, c.id, 'shiftL') },
    { label: 'Shift right', action: () => A.stepOp(patId, c.id, 'shiftR') },
    { label: 'Invert', action: () => A.stepOp(patId, c.id, 'invert') },
    { label: 'Randomize', action: () => A.stepOp(patId, c.id, 'random') },
    { label: 'Clear', action: () => A.stepOp(patId, c.id, 'clear') },
    { label: 'Copy steps', action: () => A.copySteps(patId, c.id) },
    { label: 'Paste steps', action: () => A.pasteSteps(patId, c.id) },
    { sep: true, label: '' },
    { label: 'Move up', action: () => A.moveChannel(c.id, idxOf(c.id) - 1) },
    { label: 'Move down', action: () => A.moveChannel(c.id, idxOf(c.id) + 1) },
    { label: 'Delete channel', danger: true, action: () => confirm(`Delete channel "${c.name}"?`) && A.deleteChannel(c.id) },
  ];
}
const idxOf = (id: number) => proj.state.project?.channels.findIndex((c) => c.id === id) ?? 0;

export function openEditor(channel: number) {
  ui.set((s) => ({ selectedChannel: channel, editor: { kind: 'channel', id: channel }, panels: { ...s.panels, plugin: true } }));
}
export function openPiano(channel: number) {
  ui.set((s) => ({ selectedChannel: channel, pianoChannel: channel, panels: { ...s.panels, piano: true }, focus: 'piano', mobileTab: 'piano' }));
}

function PlayingStep(p: { count: number; patId: number }) {
  // re-renders at the status rate only for this indicator
  const idx = useStore(live, (s) => {
    const st = s.status;
    if (!st || st.play !== 1 || st.mode !== 'pattern' || st.pattern !== p.patId) return -1;
    return Math.floor(st.tick / STEP_TICKS) % p.count;
  });
  return <style>{idx >= 0 ? `.steps[data-pat="${p.patId}"] .step:nth-of-type(${idx + 1}){outline:1px solid #fff;outline-offset:-1px}` : ''}</style>;
}

function StepRow(p: { c: Channel; pat: Pattern; graph: boolean; mobile: boolean }) {
  const seq = A.seqOf(p.pat, p.c.id);
  const steps: StepArr[] = seq?.steps ?? A.emptySteps(16);
  const [draft, setDraft] = useState<StepArr[] | null>(null);
  const shown = draft ?? steps;
  const g = useRef<{ target: number; mode: 'toggle' | 'vel'; before: StepArr[]; work: StepArr[] } | null>(null);
  // hit-test against the rendered pads, so gaps and phone / desktop sizes need no arithmetic here
  const indexAt = (e: PointerEvent) => {
    const pads = (e.currentTarget as HTMLElement).querySelectorAll('.step');
    let best = 0, bd = Infinity;
    pads.forEach((el, i) => {
      const r = el.getBoundingClientRect();
      const d = e.clientX < r.left ? r.left - e.clientX : e.clientX > r.right ? e.clientX - r.right : 0;
      if (d < bd) (bd = d), (best = i);
    });
    return Math.min(best, shown.length - 1);
  };
  const velAt = (e: PointerEvent) => {
    const t = (e.target as HTMLElement).closest('.step')?.getBoundingClientRect();
    return t ? Math.max(0.05, Math.min(1, 1 - (e.clientY - t.top) / t.height)) : 0.78;
  };
  const onDown = (e: PointerEvent) => {
    if (e.button === 2) return;
    const i = indexAt(e);
    const work = steps.map((x) => [...x] as StepArr);
    if (p.graph) {
      g.current = { target: 1, mode: 'vel', before: steps, work };
      work[i][0] = 1;
      work[i][1] = velAt(e);
    } else {
      const target = work[i][0] ? 0 : 1;
      g.current = { target, mode: 'toggle', before: steps, work };
      work[i][0] = target;
      if (e.shiftKey || e.altKey) {
        work[i][0] = 1;
        work[i][4] = work[i][4] ? 0 : 1;
      }
      if (target) A.previewNote(p.c.id, work[i][3] || p.c.root, 150);
    }
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    setDraft(work);
  };
  const onMove = (e: PointerEvent) => {
    const gs = g.current;
    if (!gs) return;
    const i = indexAt(e);
    if (gs.mode === 'vel') {
      gs.work[i][0] = 1;
      gs.work[i][1] = velAt(e);
    } else if (gs.work[i][0] !== gs.target) {
      gs.work[i][0] = gs.target;
    } else return;
    setDraft(gs.work.map((x) => [...x] as StepArr));
  };
  const onUp = () => {
    const gs = g.current;
    g.current = null;
    if (!gs) return;
    setDraft(null);
    A.setSteps(p.pat.id, p.c.id, gs.work, p.graph ? 'Step velocity' : 'Paint steps', seq?.stepCount ?? gs.work.length);
  };
  const stepMenu = (e: PointerEvent | MouseEvent, i: number) => {
    e.preventDefault();
    const s = steps[i];
    const set = (f: (x: StepArr) => void) => {
      const work = steps.map((x) => [...x] as StepArr);
      f(work[i]);
      A.setSteps(p.pat.id, p.c.id, work, 'Edit step', seq?.stepCount ?? work.length);
    };
    showMenu(
      [
        { label: s[0] ? 'Remove step' : 'Add step', action: () => set((x) => (x[0] = x[0] ? 0 : 1)) },
        { label: 'Accent', checked: !!s[4], action: () => set((x) => { x[0] = 1; x[4] = x[4] ? 0 : 1; }) },
        { label: 'Velocity 100 %', action: () => set((x) => { x[0] = 1; x[1] = 1; }) },
        { label: 'Velocity 50 %', action: () => set((x) => { x[0] = 1; x[1] = 0.5; }) },
        { label: 'Probability 100 %', checked: s[5] >= 1, action: () => set((x) => (x[5] = 1)) },
        { label: 'Probability 50 %', checked: s[5] === 0.5, action: () => set((x) => (x[5] = 0.5)) },
        { label: 'Probability 25 %', checked: s[5] === 0.25, action: () => set((x) => (x[5] = 0.25)) },
        { label: 'Pitch: channel root', action: () => set((x) => (x[3] = 0)) },
        { label: 'Pitch +12', action: () => set((x) => (x[3] = (x[3] || p.c.root) + 12)) },
        { label: 'Pitch -12', action: () => set((x) => (x[3] = Math.max(0, (x[3] || p.c.root) - 12))) },
      ],
      e.clientX,
      e.clientY,
    );
  };
  return (
    <div
      class="steps"
      data-pat={p.pat.id}
      style={{ touchAction: 'none' }}
      onPointerDown={onDown}
      onPointerMove={onMove}
      onPointerUp={onUp}
      onPointerCancel={onUp}
      onContextMenu={(e) => {
        const i = indexAt(e as unknown as PointerEvent);
        stepMenu(e, i);
      }}
    >
      {shown.map((s, i) => (
        <>
          <button
            type="button"
            key={i}
            class={`step${Math.floor(i / 4) % 2 ? ' alt' : ''}${s[0] ? ' on' : ''}${s[4] ? ' accent' : ''}`}
            style={p.mobile ? { width: '30px', height: '36px' } : {}}
            aria-label={`step ${i + 1}${s[0] ? ' on' : ''}`}
            title={s[0] ? `step ${i + 1}: velocity ${Math.round(s[1] * 100)} %${s[5] < 1 ? `, probability ${Math.round(s[5] * 100)} %` : ''}` : `step ${i + 1}`}
          >
            {p.graph && s[0] ? <span class="vel" style={{ height: `${Math.round(s[1] * 20)}px` }} /> : null}
            {s[5] < 1 && s[0] ? <span style={{ position: 'absolute', top: '1px', left: '2px', fontSize: '8px', color: '#000' }}>?</span> : null}
          </button>
          {i % 4 === 3 ? <span class="step-gap" /> : null}
        </>
      ))}
    </div>
  );
}

function MiniRoll(p: { c: Channel; pat: Pattern; width: number }) {
  const seq = A.seqOf(p.pat, p.c.id);
  const notes = (seq?.notes ?? []).map(toNote);
  const len = Math.max(p.pat.effLength || 3840, 1);
  let lo = 127, hi = 0;
  for (const n of notes) {
    lo = Math.min(lo, n.key);
    hi = Math.max(hi, n.key);
  }
  const range = Math.max(12, hi - lo + 1);
  return (
    <svg class="mini-roll" width={p.width} height={24} onClick={() => openPiano(p.c.id)} role="button" aria-label="Open the piano roll">
      {notes.map((n) => (
        <rect
          key={n.id}
          x={(n.start / len) * p.width}
          y={22 - ((n.key - lo) / range) * 20 - 2}
          width={Math.max(2, (n.len / len) * p.width - 1)}
          height={2}
          fill={p.c.color}
        />
      ))}
    </svg>
  );
}

function RackRow(p: { c: Channel; pat: Pattern; graph: boolean; mobile: boolean; level: number }) {
  const { c, pat } = p;
  const sel = useStore(ui, (s) => s.selectedChannel === c.id);
  const seq = A.seqOf(pat, c.id);
  const lp = longPress((e) => showMenu(channelMenu(c, pat), e.clientX, e.clientY));
  const stepW = (seq?.stepCount ?? 16) * (p.mobile ? 32 : 20) + Math.floor((seq?.stepCount ?? 16) / 4) * 4;
  return (
    <div class={`rack-row${sel ? ' sel' : ''}`} onPointerDown={() => ui.set({ selectedChannel: c.id, pianoChannel: c.id })}>
      <div class="ch-ctl">
        <Led on={!c.mute} title={c.mute ? 'Unmute' : 'Mute'} onClick={() => A.setChannel(c.id, { mute: !c.mute })} />
        {!p.mobile ? (
          <>
            <Knob size={22} value={c.pan} min={-1} max={1} def={0} scale="bipolar" title={`Pan ${Math.round(c.pan * 100)}`} onChange={(v, f) => A.setChannel(c.id, { pan: v }, f)} />
            <Knob size={22} value={c.vol} min={0} max={1.25} def={0.78} title={`Volume ${Math.round(c.vol * 100)} %`} color="#5fd068" onChange={(v, f) => A.setChannel(c.id, { vol: v }, f)} />
            <button type="button" class="ch-track" title="Mixer track" onClick={(e) => menuAt(e as MouseEvent, Array.from({ length: 17 }, (_, i) => ({ label: i ? `Insert ${i}` : 'Master', checked: c.track === i, action: () => A.setChannel(c.id, { track: i }) })))}>
              {c.track || 'M'}
            </button>
          </>
        ) : null}
      </div>
      <button
        type="button"
        class={`ch-name${c.missing ? ' missing' : ''}`}
        style={{ ['--chc' as string]: c.color, ...(p.level > 0.01 ? { boxShadow: `inset 3px 0 0 ${c.color}, inset 0 -2px 0 rgba(95,208,104,${Math.min(1, p.level * 2)})` } : {}) }}
        title={c.missing ? `Plugin ${c.plugin.id} is not in this firmware (kept in the project)` : `${c.name}: click to open, right-click / long-press for the menu`}
        onClick={() => openEditor(c.id)}
        onContextMenu={(e) => {
          e.preventDefault();
          showMenu(channelMenu(c, pat), e.clientX, e.clientY);
        }}
        {...lp}
      >
        {c.solo ? <span style={{ color: 'var(--accent)' }}>S</span> : null}
        {c.name}
      </button>
      {seq?.pianoRoll ? <MiniRoll c={c} pat={pat} width={stepW} /> : <StepRow c={c} pat={pat} graph={p.graph} mobile={p.mobile} />}
      {!seq?.pianoRoll ? (
        <DragNumber value={seq?.stepCount ?? 16} min={1} max={128} width={30} title="Steps in this row (independent lengths = polymeter)" onChange={(v, f) => f && A.setStepCount(pat.id, c.id, Math.round(v))} />
      ) : (
        <Btn class="icon flat" title="Piano roll" onClick={() => openPiano(c.id)}>
          <Icon name="piano" />
        </Btn>
      )}
    </div>
  );
}

export function ChannelRack(p: { mobile?: boolean }) {
  const project = useStore(proj, (s) => s.project);
  const levels = useStore(live, (s) => s.status?.cm ?? []);
  const [graph, setGraph] = useState(false);
  if (!project) return null;
  const pat = project.patterns.find((x) => x.id === project.meta.currentPattern) ?? project.patterns[0];
  if (!pat) return null;
  const maxSteps = Math.max(16, ...pat.seqs.map((s) => s.stepCount));
  return (
    <div class="panel" style={{ flex: 1 }}>
      <div class="panel-head">
        <span class="panel-title">Channel rack</span>
        {!p.mobile ? <PatternSelector compact /> : null}
        <Knob size={22} value={pat.swing} min={0} max={1} def={0} title={`Swing ${Math.round(pat.swing * 100)} %`} onChange={(v, f) => f && A.setPattern(pat.id, { swing: v })} />
        <span class="hint">swing</span>
        <Btn class="icon" active={graph} title="Graph: drag on steps to set their velocity" onClick={() => setGraph(!graph)}>
          VEL
        </Btn>
        <div class="spacer" />
        <Btn class="icon" title="Add a channel" onClick={(e) => menuAt(e, instrumentMenu((id) => A.addChannel(id)))}>
          <Icon name="plus" /> {p.mobile ? '' : 'Channel'}
        </Btn>
        {!p.mobile ? (
          <Btn class="icon flat" title="Close" onClick={() => togglePanel('rack')}>
            <Icon name="x" />
          </Btn>
        ) : null}
      </div>
      <div class="panel-body">
        <div class="rack">
          <PlayingStep count={maxSteps} patId={pat.id} />
          {project.channels.map((c, i) => (
            <RackRow key={c.id} c={c} pat={pat} graph={graph} mobile={!!p.mobile} level={levels[i] ?? 0} />
          ))}
          {project.channels.length === 0 ? <div class="hint" style={{ padding: '12px' }}>No channels: add an instrument with “+ Channel”.</div> : null}
          <div class="rack-foot">
            <Btn onClick={(e) => menuAt(e, instrumentMenu((id) => A.addChannel(id).then((nid) => nid && toast('Channel added'))))}>
              <Icon name="plus" /> Add channel
            </Btn>
          </div>
        </div>
      </div>
    </div>
  );
}
