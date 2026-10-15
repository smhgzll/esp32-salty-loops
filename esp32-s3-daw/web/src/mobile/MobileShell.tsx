// SPDX-License-Identifier: GPL-3.0-or-later
// Touch workspace for phones and small tablets: a compact transport, one full-screen editor at a time
// (song, patterns, piano roll, mixer, instrument, more) and a bottom tab bar. Menus open as bottom
// sheets; nothing depends on hover or a physical keyboard.
import { useState } from 'preact/hooks';

import * as A from '../app/actions';
import { live, proj, ui } from '../app/state';
import type { MobileTab } from '../app/state';
import { useStore } from '../app/store';
import { fmtBbt } from '../app/util';
import { ChannelRack, instrumentMenu, openEditor } from '../channel-rack/ChannelRack';
import { Mixer } from '../mixer/Mixer';
import { PianoRoll } from '../piano-roll/PianoRoll';
import { Playlist } from '../playlist/Playlist';
import { EditorBody } from '../plugins/PluginEditor';
import { exportProject, importProjectFile, openProjectDialog } from '../settings/ProjectDialogs';
import { importMidi } from '../midi/MidiImport';
import { Settings } from '../settings/Settings';
import { PatternSelector } from '../transport/TopBar';
import { Btn, DragNumber, Icon, showMenu } from '../ui/controls';
import { VirtualKeyboard } from '../ui/Keyboard';

const TABS: { id: MobileTab; label: string; icon: string }[] = [
  { id: 'song', label: 'Song', icon: 'song' },
  { id: 'patterns', label: 'Patterns', icon: 'steps' },
  { id: 'piano', label: 'Piano', icon: 'piano' },
  { id: 'mixer', label: 'Mixer', icon: 'mixer' },
  { id: 'plugins', label: 'Sound', icon: 'plug' },
  { id: 'more', label: 'More', icon: 'more' },
];

function Sheet(p: { onClose: () => void; children: preact.ComponentChildren }) {
  return (
    <>
      <div class="sheet-back" onPointerDown={p.onClose} />
      <div class="sheet" role="dialog">
        <div class="sheet-grip" />
        {p.children}
      </div>
    </>
  );
}

function MTop() {
  const st = useStore(live, (s) => [s.status?.play ?? 0, s.status?.tick ?? 0, s.status?.mode ?? 'pattern'] as const);
  const meta = useStore(proj, (s) => s.project?.meta);
  if (!meta) return null;
  return (
    <div class="mtop">
      <Btn class="icon green" active={st[0] === 1} title="Play / pause" onClick={A.togglePlay}>
        <Icon name={st[0] === 1 ? 'pause' : 'play'} size={20} />
      </Btn>
      <Btn class="icon" title="Stop" onClick={A.stop}>
        <Icon name="stop" size={20} />
      </Btn>
      <Btn class="icon" active={st[2] === 'song'} title="Pattern / song mode" onClick={() => A.setMode(st[2] === 'song' ? 'pattern' : 'song')}>
        {st[2] === 'song' ? 'SONG' : 'PAT'}
      </Btn>
      <DragNumber value={meta.bpm} min={20} max={300} decimals={0} width={46} title="Tempo" onChange={A.setBpm} />
      <div class="lcd" style={{ flex: 1, overflow: 'hidden' }}>{fmtBbt(st[1], meta.ppq, meta.timeSig).trim()}</div>
    </div>
  );
}

function SoundTab() {
  const project = useStore(proj, (s) => s.project);
  const sel = useStore(ui, (s) => s.selectedChannel);
  const [pick, setPick] = useState(false);
  if (!project) return null;
  return (
    <div class="mlist">
      <div class="row" style={{ padding: '8px' }}>
        <select
          value={sel}
          aria-label="Channel"
          style={{ flex: 1, minHeight: '36px' }}
          onChange={(e) => {
            const id = +(e.target as HTMLSelectElement).value;
            ui.set({ selectedChannel: id, pianoChannel: id });
            openEditor(id);
          }}
        >
          {project.channels.map((c) => (
            <option key={c.id} value={c.id}>
              {c.name}
            </option>
          ))}
        </select>
        <Btn onClick={() => setPick(true)}>
          <Icon name="plus" /> Channel
        </Btn>
      </div>
      <div style={{ padding: '0 8px 8px' }}>
        <EditorBody />
      </div>
      {pick ? (
        <Sheet onClose={() => setPick(false)}>
          {instrumentMenu((id) => {
            setPick(false);
            A.addChannel(id);
          }).map((m) => (
            <div class="mrow" key={m.label} onClick={m.action}>
              <Icon name="wave" /> {m.label}
            </div>
          ))}
        </Sheet>
      ) : null}
    </div>
  );
}

function MoreTab() {
  const [settings, setSettings] = useState(false);
  if (settings) return <Settings mobile />;
  const rows: [string, string, () => void][] = [
    ['folder', 'Open project…', () => openProjectDialog('open')],
    ['save', 'Save project as…', () => openProjectDialog('save')],
    ['plus', 'New project (demo)', () => confirm('Start a new demo project?') && A.newProject('demo')],
    ['plus', 'New empty project', () => confirm('Start a new empty project?') && A.newProject('empty')],
    ['dup', 'Export to a file', exportProject],
    ['folder', 'Import a file…', importProjectFile],
    ['import', 'Import a MIDI file…', () => importMidi('project')],
    ['metro', 'Metronome', () => A.setMetronome(!proj.state.project?.meta.metronome)],
    ['loop', 'Song loop on / off', () => A.setLoop(!proj.state.project?.meta.loop.on)],
    ['gear', 'Device & diagnostics', () => setSettings(true)],
    ['power', 'Panic (all sound off)', A.panic],
  ];
  return (
    <div class="mlist">
      {rows.map(([icon, label, act]) => (
        <div class="mrow" key={label} onClick={act}>
          <Icon name={icon} size={20} /> {label}
        </div>
      ))}
    </div>
  );
}

export function MobileShell() {
  const tab = useStore(ui, (s) => s.mobileTab);
  const kbd = useStore(ui, (s) => s.keyboard);
  const landscape = window.innerWidth > window.innerHeight;
  return (
    <div class="mshell">
      <MTop />
      {tab === 'patterns' || tab === 'piano' ? (
        <div class="row" style={{ padding: '4px 6px', background: 'var(--bg2)', borderBottom: '1px solid #000', flexWrap: 'nowrap' }}>
          <PatternSelector compact />
          <div class="spacer" />
          <Btn class="icon" active={kbd} title="Keyboard" onClick={() => ui.set((s) => ({ keyboard: !s.keyboard }))}>
            <Icon name="kbd" size={20} />
          </Btn>
          <Btn class="icon" title="Pattern options" onClick={(e) => showMenu([
            { label: 'New pattern', action: () => A.addPattern().then((id) => A.selectPattern(id, false)) },
            { label: 'Clone pattern', action: () => A.dupPattern(proj.state.project!.meta.currentPattern) },
          ], e.clientX, e.clientY)}>
            <Icon name="more" size={20} />
          </Btn>
        </div>
      ) : null}
      <div class="mmain">
        {tab === 'song' ? <Playlist mobile /> : null}
        {tab === 'patterns' ? <ChannelRack mobile /> : null}
        {tab === 'piano' ? <PianoRoll mobile /> : null}
        {tab === 'mixer' ? <Mixer mobile height={Math.max(240, window.innerHeight - (landscape ? 120 : 170))} /> : null}
        {tab === 'plugins' ? <SoundTab /> : null}
        {tab === 'more' ? <MoreTab /> : null}
      </div>
      {kbd && (tab === 'piano' || tab === 'patterns') ? <VirtualKeyboard height={landscape ? 70 : 110} /> : null}
      <nav class="mtabs">
        {TABS.map((t) => (
          <button key={t.id} type="button" class={`mtab${tab === t.id ? ' on' : ''}`} aria-current={tab === t.id} onClick={() => ui.set({ mobileTab: t.id })}>
            <Icon name={t.icon} size={landscape ? 18 : 22} />
            {t.label}
          </button>
        ))}
      </nav>
    </div>
  );
}
