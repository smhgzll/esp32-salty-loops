// SPDX-License-Identifier: GPL-3.0-or-later
// WebSocket client for protocol 1 (docs/PROTOCOL.md): handshake, reconnect with backoff, full
// resynchronisation from the welcome snapshot, revisioned patches, requests with acks, and
// coalesced continuous controls (knobs / faders send at most every 30 ms, then the final value).
import { catalog, conn, live, proj, samples, slots, toast, ui, updateProject, userPresets } from './state';
import type { Automation, Channel, Meta, Pattern, Playlist, Project, Track } from './types';

type Ack = { t: 'ack' | 'err'; id: number; rev: number; code?: string; msg?: string; [k: string]: unknown };

let ws: WebSocket | null = null;
let nextId = 1;
const pending = new Map<number, { resolve: (a: Ack) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> }>();
let backoff = 500;
let pingTimer: ReturnType<typeof setInterval> | null = null;

function url() {
  const loc = window.location;
  return `${loc.protocol === 'https:' ? 'wss' : 'ws'}://${loc.host}/ws`;
}

export function connect() {
  conn.set((s) => ({ state: 'connecting', attempts: s.attempts + 1 }));
  try {
    ws = new WebSocket(url());
  } catch {
    scheduleReconnect();
    return;
  }
  ws.onopen = () => {
    backoff = 500;
    ws!.send(JSON.stringify({ t: 'hello', proto: 1, client: 'web', rev: conn.state.rev }));
  };
  ws.onmessage = (ev) => {
    let m: any;
    try {
      m = JSON.parse(ev.data as string);
    } catch {
      return;
    }
    handle(m);
  };
  ws.onclose = () => {
    for (const [, p] of pending) {
      clearTimeout(p.timer);
      p.reject(new Error('disconnected'));
    }
    pending.clear();
    conn.set({ state: 'closed' });
    if (pingTimer) clearInterval(pingTimer);
    scheduleReconnect();
  };
}

function scheduleReconnect() {
  setTimeout(connect, backoff);
  backoff = Math.min(backoff * 1.6, 5000);
}

// The device leaves out what a pattern row has by default (project_json.cpp, patternJson): steps
// that are all off, and note fields after the velocity that hold their defaults. Filled in here,
// so the rest of the UI always sees complete rows.
const NOTE_DEFAULTS = [0, 0, 240, 60, 0.78, 0, 0, 0.5, 1, 0];
function fullPattern(pat: Pattern): Pattern {
  for (const q of pat.seqs ?? []) {
    if (!q.steps) q.steps = Array.from({ length: q.stepCount ?? 16 }, () => [0, 0.78, 0, 0, 0, 1] as Pattern['seqs'][number]['steps'][number]);
    q.notes = (q.notes ?? []).map((n) => (n.length < 10 ? ([...n, ...NOTE_DEFAULTS.slice(n.length)] as typeof n) : n));
  }
  return pat;
}

function handle(m: any) {
  switch (m.t) {
    case 'welcome':
      conn.set({ state: 'open', server: m.server, clientId: m.client, rev: m.rev });
      catalog.set({ plugins: m.plugins });
      slots.set({ list: m.projects ?? [] });
      samples.set({ library: m.samples ?? { list: [] } });
      userPresets.set({ list: m.userPresets ?? [] });
      (m.project as Project).patterns.forEach(fullPattern);
      proj.set({ project: m.project as Project });
      {
        const p = m.project as Project;
        const s = ui.state;
        const sel = p.channels.find((c) => c.id === s.selectedChannel) ? s.selectedChannel : p.channels[0]?.id ?? 0;
        // the piano roll opens on a channel that has notes somewhere
        const withNotes = p.patterns.flatMap((x) => x.seqs).find((q) => q.pianoRoll && q.notes.length)?.channel;
        const pc = p.channels.find((c) => c.id === s.pianoChannel) ? s.pianoChannel : withNotes ?? sel;
        ui.set({ selectedChannel: sel, pianoChannel: pc });
      }
      if (pingTimer) clearInterval(pingTimer);
      pingTimer = setInterval(measureLatency, 5000);
      break;
    case 'ack':
    case 'err': {
      if (m.rev) conn.set({ rev: m.rev });
      const p = pending.get(m.id);
      if (p) {
        clearTimeout(p.timer);
        pending.delete(m.id);
        p.resolve(m as Ack);
      }
      if (m.t === 'err') toast(`${m.op ?? 'error'}: ${m.msg ?? m.code}`, 'error');
      break;
    }
    case 'patch':
      conn.set({ rev: m.rev });
      applyPatch(m.path, m.value);
      break;
    case 'set':
      conn.set({ rev: m.rev });
      applySet(m);
      break;
    case 'st':
      live.set({ status: m });
      break;
    case 'diag':
      live.set({ diag: m });
      break;
    case 'projects':
      slots.set({ list: m.list });
      break;
    case 'samples':
      samples.set({ library: m.library });
      break;
    case 'userPresets':
      userPresets.set({ list: m.list });
      break;
  }
}

function applyPatch(path: string, v: any) {
  updateProject((p) => {
    switch (path) {
      case 'meta':
        return { ...p, meta: v as Meta };
      case 'channels':
        return { ...p, channels: v as Channel[] };
      case 'patternList': {
        // metadata and order; keep the sequences we have
        const list = (v as Pattern[]).map((meta) => {
          const old = p.patterns.find((x) => x.id === meta.id);
          return { ...(old ?? { seqs: [] }), ...meta } as Pattern;
        });
        return { ...p, patterns: list };
      }
      case 'pattern': {
        const pat = fullPattern(v as Pattern);
        const exists = p.patterns.some((x) => x.id === pat.id);
        return { ...p, patterns: exists ? p.patterns.map((x) => (x.id === pat.id ? pat : x)) : [...p.patterns, pat] };
      }
      case 'midi': {
        return { ...p, patterns: p.patterns.map((pat) => {
          if (pat.id !== v.pattern) return pat;
          const old = pat.seqs.find((q) => q.channel === v.channel);
          const row = old ?? { channel: v.channel, stepCount: 16, pianoRoll: false, nextNoteId: 1, notes: [], midi: [] as number[][], steps: Array.from({ length: 16 }, () => [0, .78, 0, 0, 0, 1]) };
          const midi = [...(row.midi ?? []).slice(0, v.from), ...v.events].slice(0, v.total);
          const next = fullPattern({ ...pat, effLength: v.effLength, seqs: [...pat.seqs.filter((q) => q.channel !== v.channel), { ...row, midi }] } as Pattern);
          return next;
        }) };
      }
      case 'mixer':
        return { ...p, mixer: v as Track[] };
      case 'track':
        return { ...p, mixer: p.mixer.map((t) => (t.index === (v as Track).index ? (v as Track) : t)) };
      case 'playlist':
        return { ...p, playlist: v as Playlist };
      case 'automation':
        return { ...p, automation: v as Automation[] };
      default:
        return p;
    }
  });
}

function applySet(m: any) {
  if (m.by === conn.state.clientId) return; // our own change, already applied locally
  const parts = (m.path as string).split('/');
  updateProject((p) => {
    if (parts[0] === 'channel') {
      const id = +parts[1];
      return {
        ...p,
        channels: p.channels.map((c) => {
          if (c.id !== id) return c;
          if (parts[2] === 'param') return { ...c, plugin: { ...c.plugin, params: { ...c.plugin.params, [m.key]: m.v } } };
          return { ...c, vol: m.vol ?? c.vol, pan: m.pan ?? c.pan };
        }),
      };
    }
    if (parts[0] === 'track') {
      const i = +parts[1];
      return { ...p, mixer: p.mixer.map((t) => (t.index === i ? { ...t, vol: m.vol ?? t.vol, pan: m.pan ?? t.pan } : t)) };
    }
    if (parts[0] === 'fx' && parts[2] === 'param') {
      const id = +parts[1];
      return {
        ...p,
        mixer: p.mixer.map((t) => ({
          ...t,
          fx: t.fx.map((f) => (f.id === id ? { ...f, plugin: { ...f.plugin, params: { ...f.plugin.params, [m.key]: m.v } } } : f)),
        })),
      };
    }
    return p;
  });
}

/** send an operation and wait for its ack (rejects on err / timeout / disconnect) */
export function req(t: string, args: Record<string, unknown> = {}, timeoutMs = 10000): Promise<Ack> {
  return new Promise((resolve, reject) => {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      reject(new Error('not connected'));
      return;
    }
    const id = nextId++;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error('timeout'));
    }, timeoutMs);
    pending.set(id, { resolve, reject, timer });
    ws.send(JSON.stringify({ t, id, ...args }));
  });
}

/** fire and forget (errors still show as toasts) */
export function send(t: string, args: Record<string, unknown> = {}) {
  req(t, args).catch(() => {});
}

// continuous controls: the newest value per key, at most every 30 ms
const throttled = new Map<string, { last: number; timer: ReturnType<typeof setTimeout> | null; msg: [string, Record<string, unknown>] }>();
export function sendControl(key: string, t: string, args: Record<string, unknown>, final = false) {
  let e = throttled.get(key);
  if (!e) {
    e = { last: 0, timer: null, msg: [t, args] };
    throttled.set(key, e);
  }
  e.msg = [t, args];
  const flush = () => {
    e!.timer = null;
    e!.last = Date.now();
    send(e!.msg[0], e!.msg[1]);
  };
  const wait = 30 - (Date.now() - e.last);
  if (final || wait <= 0) {
    if (e.timer) clearTimeout(e.timer);
    flush();
  } else if (!e.timer) e.timer = setTimeout(flush, wait);
}

async function measureLatency() {
  const t0 = performance.now();
  try {
    await req('ping', { ts: Date.now() }, 4000);
    conn.set({ latencyMs: Math.round(performance.now() - t0) });
  } catch {
    /* reconnect logic handles it */
  }
}

// ---- privileged HTTP endpoints ----------------------------------------------------------------
export async function login(password: string) {
  const r = await fetch('/api/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ password }) });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error ?? `login failed (${r.status})`);
  ui.set({ token: j.token });
  return j.token as string;
}
export async function authedGet(path: string) {
  const r = await fetch(path, { headers: { Authorization: `Bearer ${ui.state.token}` } });
  const j = await r.json().catch(() => ({}));
  if (r.status === 401) ui.set({ token: '' });
  if (!r.ok) throw new Error(j.error ?? `request failed (${r.status})`);
  return j;
}
export async function authed(path: string, body: BodyInit, type = 'application/json') {
  const r = await fetch(path, { method: 'POST', headers: { 'Content-Type': type, Authorization: `Bearer ${ui.state.token}` }, body });
  const j = await r.json().catch(() => ({}));
  if (r.status === 401) ui.set({ token: '' });
  if (!r.ok) throw new Error(j.error ?? `request failed (${r.status})`);
  return j;
}
