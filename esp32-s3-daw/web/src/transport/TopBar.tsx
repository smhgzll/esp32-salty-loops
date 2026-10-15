// SPDX-License-Identifier: GPL-3.0-or-later
// The top bar: menus, transport, tempo, position, pattern selector, load, connection.
import * as A from '../app/actions';
import { canRedo, canUndo, history, redo, undo, undoLabel } from '../app/history';
import { conn, live, proj, ui } from '../app/state';
import type { Panel } from '../app/state';
import { useStore } from '../app/store';
import { fmtBbt } from '../app/util';
import { Btn, DragNumber, Icon, menuAt, showMenu } from '../ui/controls';
import type { MenuItem } from '../ui/controls';
import { openProjectDialog, exportProject, importProjectFile } from '../settings/ProjectDialogs';
import { importMidi } from '../midi/MidiImport';
import { importModule } from '../tracker/ModuleImport';
import { useRef } from 'preact/hooks';

export function togglePanel(p: Panel, only = false) {
  ui.set((s) => ({ panels: only ? { ...s.panels, [p]: true } : { ...s.panels, [p]: !s.panels[p] }, focus: p }));
}

export function fileMenu(): MenuItem[] {
  return [
    { label: 'New (demo song)', action: () => A.newProject('demo') },
    { label: 'New (empty)', action: () => A.newProject('empty') },
    { sep: true, label: '' },
    { label: 'Open…', action: () => openProjectDialog('open'), shortcut: 'Ctrl+O' },
    { label: 'Save as…', action: () => openProjectDialog('save'), shortcut: 'Ctrl+S' },
    { sep: true, label: '' },
    { label: 'Export project file', action: exportProject },
    { label: 'Import project file…', action: importProjectFile },
    { label: 'Import MIDI file…', action: () => importMidi('project') },
    { label: 'Import tracker module (MOD, XM, S3M, IT)…', action: () => importModule() },
  ];
}

function viewMenu(): MenuItem[] {
  const s = ui.state.panels;
  const v = (p: Panel, label: string, key: string): MenuItem => ({ label, checked: s[p], shortcut: key, action: () => togglePanel(p) });
  return [
    v('playlist', 'Playlist', 'F5'),
    v('rack', 'Channel rack', 'F6'),
    v('piano', 'Piano roll', 'F7'),
    v('browser', 'Browser', 'F8'),
    v('mixer', 'Mixer', 'F9'),
    { sep: true, label: '' },
    { label: 'Virtual keyboard', checked: ui.state.keyboard, action: () => ui.set((x) => ({ keyboard: !x.keyboard })), shortcut: 'K' },
    { label: 'Device & diagnostics', action: () => togglePanel('settings', true) },
  ];
}

function Position() {
  const st = useStore(live, (s) => s.status);
  const meta = useStore(proj, (s) => s.project?.meta);
  if (!meta) return <div class="lcd">---:--:---</div>;
  const tick = st?.tick ?? 0;
  return (
    <div class="lcd" title="Position (bar:beat:tick)">
      {fmtBbt(tick, meta.ppq, meta.timeSig)}
      <small> {st?.play === 3 ? 'COUNT' : st?.mode === 'song' ? 'SONG' : 'PAT'}</small>
    </div>
  );
}

function Load() {
  const st = useStore(live, (s) => s.status);
  const cpu = st?.cpu ?? 0;
  // the CPU guard skipped notes in the last 3 s: the meter turns red and says so
  const skips = useRef({ count: -1, at: 0 });
  const n = st?.cpuSkips ?? 0;
  if (skips.current.count >= 0 && n > skips.current.count) skips.current.at = Date.now();
  skips.current.count = n;
  const guard = Date.now() - skips.current.at < 3000;
  const master = st ? Math.max(st.m[0] ?? 0, st.m[1] ?? 0) : 0;
  return (
    <>
      <div
        class={`cpu${guard ? ' guard' : ''}`}
        data-testid="cpu-meter"
        title={`DSP load ${cpu.toFixed(1)} % of the audio deadline, ${st?.voices ?? 0} voices${n ? ` — ${n} notes skipped by the CPU guard` : ''}`}
      >
        <div class="cpu-fill" style={{ width: `${Math.min(100, cpu)}%` }} />
        <span>
          {guard ? 'CPU FULL' : `${cpu.toFixed(0)}% · ${st?.voices ?? 0}v`}
        </span>
      </div>
      <div class="cpu" style={{ width: '40px' }} title="Master peak">
        <div class="cpu-fill" style={{ width: `${master <= 0.001 ? 0 : Math.min(100, ((20 * Math.log10(master) + 60) / 60) * 100)}%` }} />
      </div>
    </>
  );
}

function Connection() {
  const c = useStore(conn, (s) => [s.state, s.server?.name, s.server?.target, s.latencyMs] as const);
  return (
    <div class="conn" title={`${c[2] ?? ''} — round trip ${c[3]} ms`}>
      <span class={`conn-dot ${c[0]}`} />
      <span>{c[0] === 'open' ? `${c[1]} · ${c[3]}ms` : c[0] === 'connecting' ? 'connecting…' : 'offline'}</span>
    </div>
  );
}

export function PatternSelector(p: { compact?: boolean }) {
  const pats = useStore(proj, (s) => s.project?.patterns.map((x) => x.id + ':' + x.name + ':' + x.color) ?? []);
  const cur = useStore(proj, (s) => s.project?.meta.currentPattern ?? 0);
  const queued = useStore(live, (s) => s.status?.queued ?? 0);
  const list = pats.map((s) => {
    const [id, ...rest] = s.split(':');
    const color = rest.pop()!;
    return { id: +id, name: rest.join(':'), color };
  });
  const c = list.find((x) => x.id === cur) ?? list[0];
  const menu = (e: MouseEvent) =>
    menuAt(e, [
      ...list.map((x) => ({ label: x.name, checked: x.id === cur, action: () => A.selectPattern(x.id) })),
      { sep: true, label: '' },
      { label: 'New pattern', action: () => A.addPattern().then((id) => A.selectPattern(id, false)) },
      { label: 'Clone pattern', action: () => c && A.dupPattern(c.id) },
      { label: 'Rename…', action: () => c && renamePattern(c.id, c.name) },
      { label: 'Delete pattern', danger: true, disabled: list.length < 2, action: () => c && confirm(`Delete "${c.name}" and its playlist clips?`) && A.deletePattern(c.id) },
    ]);
  return (
    <div class="patsel">
      <Btn class="icon" title="Previous pattern" onClick={() => A.stepPattern(-1)}>
        <Icon name="left" />
      </Btn>
      <div class="name" style={p.compact ? { minWidth: '84px' } : {}} onClick={menu} title={queued ? 'A pattern is queued: it starts at the end of the current one' : 'Pattern'}>
        <span class="swatch" style={{ background: c?.color }} />
        <span style={{ overflow: 'hidden', textOverflow: 'ellipsis' }}>{c?.name ?? '—'}</span>
        {queued ? <span style={{ color: 'var(--accent)' }}>→{list.find((x) => x.id === queued)?.name}</span> : null}
      </div>
      <Btn class="icon" title="Next pattern" onClick={() => A.stepPattern(1)}>
        <Icon name="right" />
      </Btn>
      {!p.compact ? (
        <Btn class="icon" title="New pattern" onClick={() => A.addPattern().then((id) => A.selectPattern(id, false))}>
          <Icon name="plus" />
        </Btn>
      ) : null}
    </div>
  );
}

export function renamePattern(id: number, name: string) {
  const n = prompt('Pattern name', name);
  if (n) A.setPattern(id, { name: n });
}

export function TransportButtons(p: { big?: boolean }) {
  const st = useStore(live, (s) => s.status?.play ?? 0);
  const mode = useStore(live, (s) => s.status?.mode ?? 'pattern');
  return (
    <>
      <Btn class={`icon${mode === 'song' ? '' : ' green'}`} active={mode === 'pattern'} title="Pattern mode (loops the current pattern)" onClick={() => A.setMode('pattern')}>
        PAT
      </Btn>
      <Btn class="icon" active={mode === 'song'} title="Song mode (plays the playlist)" onClick={() => A.setMode('song')}>
        SONG
      </Btn>
      <Btn class={`icon${p.big ? ' big' : ''} green`} active={st === 1 || st === 3} title="Play / pause (Space)" onClick={A.togglePlay}>
        <Icon name={st === 1 ? 'pause' : 'play'} />
      </Btn>
      <Btn class="icon" title="Stop (back to the start)" onClick={A.stop}>
        <Icon name="stop" />
      </Btn>
      <RecordButton />
    </>
  );
}

export function TopBar() {
  const meta = useStore(proj, (s) => s.project?.meta);
  const hist = useStore(history, (h) => [h.pos, h.stack.length]);
  void hist;
  const panels = useStore(ui, (s) => s.panels);
  const btnPanel = (p: Panel, icon: string, title: string) => (
    <Btn class="icon flat" active={panels[p]} title={title} onClick={() => togglePanel(p)}>
      <Icon name={icon} />
    </Btn>
  );
  return (
    <div class="topbar">
      <div class="group">
        <Btn class="icon flat" title="Menu" onClick={(e) => menuAt(e, [...fileMenu(), { sep: true, label: '' }, ...viewMenu()])}>
          <Icon name="menu" />
        </Btn>
        <span class="brand">
          S3·DAW <small>{meta?.name}</small>
        </span>
        <Btn class="flat" onClick={(e) => menuAt(e, fileMenu())}>File</Btn>
        <Btn class="flat" onClick={(e) => menuAt(e, viewMenu())}>View</Btn>
      </div>
      <div class="group">
        <TransportButtons />
        <Btn class="icon" active={meta?.metronome} title="Metronome" onClick={() => A.setMetronome(!meta?.metronome)}>
          <Icon name="metro" />
        </Btn>
        <Btn
          class="icon"
          active={(meta?.countIn ?? 0) > 0}
          title="Count-in before playing"
          onClick={(e) =>
            menuAt(e, [0, 1, 2].map((b) => ({ label: b ? `${b} bar${b > 1 ? 's' : ''} count-in` : 'No count-in', checked: meta?.countIn === b, action: () => A.setCountIn(b) })))
          }
        >
          1·2
        </Btn>
        <Btn class="icon" active={meta?.loop.on} title="Song loop range on / off (set it on the playlist ruler)" onClick={() => A.setLoop(!meta?.loop.on)}>
          <Icon name="loop" />
        </Btn>
      </div>
      <div class="group">
        <DragNumber value={meta?.bpm ?? 120} min={20} max={300} step={1} decimals={1} width={62} title="Tempo (drag, double-click to type; shift = fine)" onChange={A.setBpm} />
        <Btn class="icon flat" title="Tap tempo" onClick={A.tapTempo}>
          TAP
        </Btn>
        <Position />
      </div>
      <div class="group">
        <PatternSelector />
      </div>
      <div class="group">
        <Btn class="icon flat" disabled={!canUndo()} title={`Undo ${undoLabel()} (Ctrl+Z)`} onClick={undo}>
          <Icon name="undo" />
        </Btn>
        <Btn class="icon flat" disabled={!canRedo()} title="Redo (Ctrl+Y)" onClick={redo}>
          <Icon name="redo" />
        </Btn>
      </div>
      <div class="group">
        {btnPanel('playlist', 'playlist', 'Playlist (F5)')}
        {btnPanel('rack', 'steps', 'Channel rack (F6)')}
        {btnPanel('piano', 'piano', 'Piano roll (F7)')}
        {btnPanel('browser', 'browser', 'Browser (F8)')}
        {btnPanel('mixer', 'mixer', 'Mixer (F9)')}
        <Btn class="icon flat" active={useStore(ui, (s) => s.keyboard)} title="Virtual keyboard (K)" onClick={() => ui.set((s) => ({ keyboard: !s.keyboard }))}>
          <Icon name="kbd" />
        </Btn>
        {btnPanel('settings', 'gear', 'Device & diagnostics')}
      </div>
      <div class="group">
        <Load />
      </div>
      <div class="spacer" />
      <Connection />
    </div>
  );
}

/** arms MIDI recording: notes played on the keyboards / MIDI go into the current pattern while it plays */
function RecordButton() {
  const rec = useStore(live, (s) => !!s.status?.rec);
  const menu = (e: MouseEvent) => {
    e.preventDefault();
    showMenu(
      A.RECORD_GRIDS.map(([label, q]) => ({ label: `Input grid: ${label}`, action: () => A.setRecord(rec, q) })),
      e.clientX,
      e.clientY,
    );
  };
  return (
    <Btn
      class={`icon rec${rec ? ' on' : ''}`}
      active={rec}
      title={rec ? 'Recording armed: play notes while the pattern plays (right-click: input grid)' : 'Record notes into the pattern (right-click: input grid)'}
      onClick={() => A.setRecord(!rec)}
      onContextMenu={menu}
    >
      <Icon name="rec" />
    </Btn>
  );
}
