// SPDX-License-Identifier: GPL-3.0-or-later
// Application shell: the desktop workspace (stacked, resizable editors around a browser and a mixer)
// or, on phones and small tablets, the touch workspace (one full-screen editor + tab bar).
import type { ComponentChildren } from 'preact';
import { useEffect, useRef, useState } from 'preact/hooks';

import * as A from './actions';
import { redo, undo } from './history';
import { conn, proj, ui } from './state';
import type { Panel } from './state';
import { useStore } from './store';
import { Browser } from '../browser/Browser';
import { ChannelRack } from '../channel-rack/ChannelRack';
import { Mixer } from '../mixer/Mixer';
import { MobileShell } from '../mobile/MobileShell';
import { PianoRoll } from '../piano-roll/PianoRoll';
import { Playlist } from '../playlist/Playlist';
import { FloatingEditor } from '../plugins/PluginEditor';
import { AutomationEditor } from '../playlist/AutomationEditor';
import { openProjectDialog } from '../settings/ProjectDialogs';
import { Settings } from '../settings/Settings';
import { TopBar, togglePanel } from '../transport/TopBar';
import { MenuHost } from '../ui/controls';
import { useComputerKeyboard, VirtualKeyboard } from '../ui/Keyboard';

function useLayout() {
  const pick = () => (window.innerWidth < 760 || (window.matchMedia('(pointer: coarse)').matches && window.innerWidth < 1000) ? 'mobile' : 'desktop');
  const [layout, setLayout] = useState(pick());
  useEffect(() => {
    const on = () => setLayout(pick());
    window.addEventListener('resize', on);
    return () => window.removeEventListener('resize', on);
  }, []);
  return layout;
}

function useShortcuts() {
  useEffect(() => {
    const keys: Record<string, Panel> = { F5: 'playlist', F6: 'rack', F7: 'piano', F8: 'browser', F9: 'mixer' };
    const h = (e: KeyboardEvent) => {
      const t = e.target as HTMLElement;
      if (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA') return;
      const ctrl = e.ctrlKey || e.metaKey;
      if (keys[e.key]) togglePanel(keys[e.key]);
      else if (e.key === ' ' && !ctrl) A.togglePlay();
      else if (ctrl && e.key.toLowerCase() === 'z') (e.shiftKey ? redo : undo)();
      else if (ctrl && e.key.toLowerCase() === 'y') redo();
      else if (ctrl && e.key.toLowerCase() === 's') openProjectDialog('save');
      else if (ctrl && e.key.toLowerCase() === 'o') openProjectDialog('open');
      else if (e.key.toLowerCase() === 'k' && !ctrl && !t.closest('.cv-wrap')) ui.set((s) => ({ keyboard: !s.keyboard }));
      else if (e.key === 'Escape') A.panic();
      else return;
      e.preventDefault();
    };
    window.addEventListener('keydown', h);
    return () => window.removeEventListener('keydown', h);
  }, []);
}

// vertical stack of panels with draggable splitters (sizes kept per panel in localStorage)
const loadSizes = (): Record<string, number> => {
  try {
    return JSON.parse(localStorage.getItem('daw.sizes') ?? '{}');
  } catch {
    return {};
  }
};
function Stack(p: { items: { key: string; node: ComponentChildren; def: number }[] }) {
  const [sizes, setSizes] = useState<Record<string, number>>(loadSizes());
  const box = useRef<HTMLDivElement>(null);
  const drag = useRef<{ a: string; b: string; y: number; sa: number; sb: number; total: number } | null>(null);
  const size = (k: string, def: number) => sizes[k] ?? def;
  const save = (s: Record<string, number>) => {
    setSizes(s);
    try {
      localStorage.setItem('daw.sizes', JSON.stringify(s));
    } catch {
      /* private mode */
    }
  };
  return (
    <div class="main" ref={box}>
      {p.items.map((it, i) => (
        <>
          {i > 0 ? (
            <div
              class="splitter"
              role="separator"
              onPointerDown={(e) => {
                (e.currentTarget as Element).setPointerCapture(e.pointerId);
                const a = p.items[i - 1], b = it;
                drag.current = { a: a.key, b: b.key, y: e.clientY, sa: size(a.key, a.def), sb: size(b.key, b.def), total: p.items.reduce((s, x) => s + size(x.key, x.def), 0) };
              }}
              onPointerMove={(e) => {
                const d = drag.current;
                if (!d || !box.current) return;
                const px = box.current.clientHeight;
                const dy = ((e.clientY - d.y) / px) * d.total;
                const sa = Math.max(0.15, d.sa + dy), sb = Math.max(0.15, d.sb - dy);
                if (sa > 0.15 && sb > 0.15) save({ ...sizes, [d.a]: sa, [d.b]: sb });
              }}
              onPointerUp={() => (drag.current = null)}
            />
          ) : null}
          <div key={it.key} style={{ flex: `${size(it.key, it.def)} 1 0`, minHeight: '60px', display: 'flex', flexDirection: 'column' }}>
            {it.node}
          </div>
        </>
      ))}
    </div>
  );
}

function Toasts() {
  const t = useStore(ui, (s) => s.toasts);
  return (
    <div class="toasts" role="status">
      {t.map((x) => (
        <div key={x.id} class={`toast ${x.kind}`}>
          {x.text}
        </div>
      ))}
    </div>
  );
}

function Splash() {
  const c = useStore(conn, (s) => [s.state, s.attempts] as const);
  return (
    <div style={{ display: 'flex', height: '100%', alignItems: 'center', justifyContent: 'center', flexDirection: 'column', gap: '10px' }}>
      <div class="brand" style={{ fontSize: '28px' }}>S3·DAW</div>
      <div class="hint">{c[0] === 'connecting' ? `connecting to the device… (attempt ${c[1]})` : 'device offline — retrying'}</div>
    </div>
  );
}

function Desktop() {
  const panels = useStore(ui, (s) => s.panels);
  const kbd = useStore(ui, (s) => s.keyboard);
  const items: { key: string; node: ComponentChildren; def: number }[] = [];
  if (panels.playlist) items.push({ key: 'playlist', node: <Playlist />, def: 1.1 });
  if (panels.rack) items.push({ key: 'rack', node: <ChannelRack />, def: 1 });
  if (panels.piano) items.push({ key: 'piano', node: <PianoRoll />, def: 1.6 });
  if (panels.settings) items.push({ key: 'settings', node: <Settings />, def: 1.2 });
  return (
    <div class="shell">
      <TopBar />
      <div class="workspace">
        {panels.browser ? <Browser /> : <div />}
        {items.length ? <Stack items={items} /> : <div class="main"><div class="hint" style={{ padding: '20px' }}>All editors are closed: F5 playlist, F6 channel rack, F7 piano roll, F9 mixer.</div></div>}
      </div>
      <div>
        {panels.mixer ? <Mixer height={Math.min(360, Math.max(260, window.innerHeight * 0.36))} /> : null}
        {kbd ? <VirtualKeyboard /> : null}
      </div>
      <FloatingEditor />
    </div>
  );
}

export function App() {
  const ready = useStore(proj, (s) => !!s.project);
  const layout = useLayout();
  useShortcuts();
  useComputerKeyboard();
  return (
    <>
      {!ready ? <Splash /> : layout === 'mobile' ? <MobileShell /> : <Desktop />}
      {ready ? <AutomationEditor /> : null}
      <MenuHost />
      <Toasts />
    </>
  );
}
