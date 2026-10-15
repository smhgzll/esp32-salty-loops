// SPDX-License-Identifier: GPL-3.0-or-later
// Application state: the project (authoritative copy on the device, mirrored here through the
// protocol), the plugin catalog, live transport / meters, and UI state.
import { Store } from './store';
import type { Channel, Diag, Pattern, PluginInfo, Project, SampleLibrary, ServerInfo, SlotInfo, Status } from './types';

export type ConnState = 'connecting' | 'open' | 'closed';
export const conn = new Store({
  state: 'connecting' as ConnState,
  server: null as ServerInfo | null,
  clientId: 0,
  rev: 0,
  attempts: 0,
  latencyMs: 0,
});
export const proj = new Store({ project: null as Project | null });
export const catalog = new Store({ plugins: [] as PluginInfo[] });
export const live = new Store({ status: null as Status | null, diag: null as Diag | null });
export const slots = new Store({ list: [] as SlotInfo[] });
export const samples = new Store({ library: { list: [] } as SampleLibrary });
export const userPresets = new Store({ list: [] as { plugin: string; name: string }[] });

export type Panel = 'playlist' | 'rack' | 'piano' | 'mixer' | 'browser' | 'plugin' | 'settings';
export type MobileTab = 'song' | 'patterns' | 'piano' | 'mixer' | 'plugins' | 'more';
export interface Toast {
  id: number;
  text: string;
  kind: 'info' | 'error';
}
export const ui = new Store({
  panels: { playlist: true, rack: true, piano: true, mixer: false, browser: true, plugin: false, settings: false } as Record<Panel, boolean>,
  focus: 'rack' as Panel,
  selectedChannel: 0,
  pianoChannel: 0,
  editor: null as { kind: 'channel' | 'fx'; id: number } | null,
  mixerTrack: 1,
  mobileTab: 'patterns' as MobileTab,
  keyboard: false,
  toasts: [] as Toast[],
  token: '' as string,
  follow: true,
  autoEdit: 0, // automation clip open in the editor (0 = none)
  rollFit: 0, // incremented to make the piano roll show its notes again (after an import)
});

let toastId = 1;
export function toast(text: string, kind: 'info' | 'error' = 'info') {
  const t = { id: toastId++, text, kind };
  ui.set((s) => ({ toasts: [...s.toasts.slice(-3), t] }));
  setTimeout(() => ui.set((s) => ({ toasts: s.toasts.filter((x) => x.id !== t.id) })), kind === 'error' ? 6000 : 3000);
}

// ---- project helpers (read) -------------------------------------------------------------------
export const P = () => proj.state.project;
export const pluginInfo = (id: string) => catalog.state.plugins.find((p) => p.id === id);
export const channelById = (id: number): Channel | undefined => P()?.channels.find((c) => c.id === id);
export const patternById = (id: number): Pattern | undefined => P()?.patterns.find((p) => p.id === id);
export const currentPattern = () => {
  const p = P();
  return p ? p.patterns.find((x) => x.id === p.meta.currentPattern) ?? p.patterns[0] : undefined;
};
export const ppq = () => P()?.meta.ppq ?? 960;
export const barTicks = () => {
  const p = P();
  const ts = p?.meta.timeSig ?? [4, 4];
  return ((ppq() * 4) / ts[1]) * ts[0];
};

// ---- project helpers (local, optimistic edits; the server's patch follows) ---------------------
export function updateProject(fn: (p: Project) => Project) {
  const p = P();
  if (p) proj.set({ project: fn(p) });
}
export function replacePattern(pat: Pattern) {
  updateProject((p) => ({ ...p, patterns: p.patterns.map((x) => (x.id === pat.id ? pat : x)) }));
}
