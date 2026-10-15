// SPDX-License-Identifier: GPL-3.0-or-later
// Instrument / effect editor generated from the plugin's parameter descriptors (no plugin code or
// markup is ever loaded into the page: a plugin can only describe controls, not inject content).
import { useEffect, useRef, useState } from 'preact/hooks';

import * as A from '../app/actions';
import { req } from '../app/protocol';
import { catalog, proj, samples, ui, userPresets } from '../app/state';
import { useStore } from '../app/store';
import type { ParamDesc, PluginInfo, PluginState, ResourceSlot } from '../app/types';
import { dbText } from '../app/util';
import { Btn, Icon, Knob, showMenu } from '../ui/controls';
import { openPiano } from '../channel-rack/ChannelRack';

/** a plain parameter value as 0..1, the inverse of the engine's automation mapping */
export const normParam = (d: ParamDesc, v: number) =>
  d.scale === 'log' && d.min > 0 ? Math.log(v / d.min) / Math.log(d.max / d.min) : d.max > d.min ? (v - d.min) / (d.max - d.min) : 0.5;

export function fmtParam(d: ParamDesc, v: number) {
  if (d.type === 'enum') return d.labels?.[Math.round(v - d.min)] ?? String(v);
  if (d.type === 'bool') return v >= 0.5 ? 'On' : 'Off';
  if (d.type === 'int') return `${v > 0 && d.scale === 'bipolar' ? '+' : ''}${Math.round(v)}${d.unit ? ` ${d.unit}` : ''}`;
  if (d.scale === 'db' && !d.unit) return `${dbText(v)} dB`;
  switch (d.unit) {
    case 'Hz': return v >= 1000 ? `${(v / 1000).toFixed(v >= 10000 ? 1 : 2)} kHz` : `${v.toFixed(v < 100 ? 1 : 0)} Hz`;
    case 'ms': return v >= 1000 ? `${(v / 1000).toFixed(2)} s` : `${v.toFixed(v < 10 ? 1 : 0)} ms`;
    case '%': return `${v.toFixed(0)} %`;
    case 'st': return `${v > 0 ? '+' : ''}${v.toFixed(1)} st`;
    case 'ct': return `${v > 0 ? '+' : ''}${v.toFixed(0)} ct`;
    case 'dB': return `${v.toFixed(1)} dB`;
    case 'oct': return `${v > 0 ? '+' : ''}${v.toFixed(2)} oct`;
    default: return d.scale === 'bipolar' ? `${v > 0 ? '+' : ''}${v.toFixed(2)}` : v.toFixed(2);
  }
}

function Param(p: { d: ParamDesc; v: number; onChange: (v: number, final: boolean) => void; onAutomate?: () => void }) {
  const { d, v } = p;
  const menu = (e: MouseEvent) => {
    e.preventDefault();
    showMenu(
      [
        { label: `Reset to ${fmtParam(d, d.def)}`, action: () => p.onChange(d.def, true) },
        { label: 'Create automation clip', disabled: !d.auto || !p.onAutomate, action: p.onAutomate },
      ],
      e.clientX,
      e.clientY,
    );
  };
  if (d.type === 'enum' && d.labels) {
    if (d.labels.length <= 4)
      return (
        <div class="pe-ctl" onContextMenu={menu}>
          <div class="row" style={{ gap: '1px' }}>
            {d.labels.map((l, i) => (
              <Btn key={i} class="icon" active={Math.round(v - d.min) === i} onClick={() => p.onChange(d.min + i, true)}>
                {l}
              </Btn>
            ))}
          </div>
          <span class="knob-label">{d.name}</span>
        </div>
      );
    return (
      <div class="pe-ctl" onContextMenu={menu}>
        <select class="mini" value={Math.round(v - d.min)} aria-label={d.name} onChange={(e) => p.onChange(d.min + +(e.target as HTMLSelectElement).value, true)}>
          {d.labels.map((l, i) => (
            <option key={i} value={i}>
              {l}
            </option>
          ))}
        </select>
        <span class="knob-label">{d.name}</span>
      </div>
    );
  }
  if (d.type === 'bool')
    return (
      <div class="pe-ctl" onContextMenu={menu}>
        <Btn active={v >= 0.5} onClick={() => p.onChange(v >= 0.5 ? 0 : 1, true)}>
          {v >= 0.5 ? 'On' : 'Off'}
        </Btn>
        <span class="knob-label">{d.name}</span>
      </div>
    );
  return (
    <div class="pe-ctl" onContextMenu={menu}>
      <Knob
        size={40}
        value={v}
        min={d.min}
        max={d.max}
        def={d.def}
        scale={d.scale === 'db' ? 'linear' : d.scale}
        step={d.type === 'int' ? 1 : undefined}
        label={d.name}
        format={(x) => fmtParam(d, x)}
        onChange={p.onChange}
      />
      <span class="pe-val">{fmtParam(d, v)}</span>
    </div>
  );
}

/** ABI 1.1: choose the library file a plugin slot plays */
function ResourcePicker(p: { slot: ResourceSlot; value: string; missing: string[]; onPick: (name: string) => void }) {
  const lib = useStore(samples, (s) => s.library);
  const exts = p.slot.extensions.split(/[ ,;]+/).filter(Boolean);
  const fits = lib.list.filter((f) => (p.slot.audio && f.type === 'wav') || (p.slot.file && exts.some((e) => f.name.toLowerCase().endsWith(`.${e}`))));
  const lost = p.value && p.missing.includes(p.value);
  return (
    <div class="row" style={{ gap: '6px', marginBottom: '6px' }}>
      <span class="hint">{p.slot.name}</span>
      <select class="mini" value={p.value} aria-label={p.slot.name} onChange={(e) => p.onPick((e.target as HTMLSelectElement).value)}>
        <option value="">— none —</option>
        {fits.map((f) => (
          <option key={f.name} value={f.name}>
            {f.name}
          </option>
        ))}
        {p.value && !fits.some((f) => f.name === p.value) ? <option value={p.value}>{p.value} (missing)</option> : null}
      </select>
      {lost ? <span class="hint" style={{ color: 'var(--red, #f66)' }}>not in the sample library: upload it under this name</span> : null}
      {!fits.length ? <span class="hint">upload files in the browser's Samples tab</span> : null}
    </div>
  );
}

type Sf2Preset = { name: string; bank: number; program: number };

/** the presets inside the chosen SoundFont (read by the device from the file's headers), for
 *  plugins that select them with "bank" / "program" parameters */
function Sf2PresetPicker(p: { file: string; bank: number; program: number; onPick: (bank: number, program: number) => void }) {
  const [list, setList] = useState<Sf2Preset[] | null>(null);
  const [error, setError] = useState('');
  useEffect(() => {
    let live = true;
    setList(null);
    setError('');
    if (p.file)
      req('samples.presets', { name: p.file })
        .then((a) => {
          if (!live) return;
          if (a.t === 'ack') setList((a.presets as Sf2Preset[]) ?? []);
          else setError(a.msg ?? 'unreadable');
        })
        .catch(() => live && setError('not available'));
    return () => {
      live = false;
    };
  }, [p.file]);
  if (!p.file) return null;
  if (error) return <div class="hint">font presets: {error}</div>;
  if (!list) return <div class="hint">reading the font's presets…</div>;
  const cur = `${p.bank}:${p.program}`;
  const known = list.some((x) => `${x.bank}:${x.program}` === cur);
  return (
    <div class="row" style={{ gap: '6px', marginBottom: '6px' }}>
      <span class="hint">Font preset</span>
      <select
        class="mini"
        value={cur}
        aria-label="SoundFont preset"
        onChange={(e) => {
          const [b, g] = (e.target as HTMLSelectElement).value.split(':').map(Number);
          p.onPick(b, g);
        }}
      >
        {known ? null : <option value={cur}>{`${p.bank}:${p.program} (not in this file)`}</option>}
        {list.map((x) => (
          <option key={`${x.bank}:${x.program}`} value={`${x.bank}:${x.program}`}>
            {`${String(x.bank).padStart(3, '0')}:${String(x.program).padStart(3, '0')} ${x.name}`}
          </option>
        ))}
      </select>
      <span class="hint">{list.length} presets</span>
    </div>
  );
}

export function PluginPanel(p: { info: PluginInfo | undefined; state: PluginState; missing?: boolean; resources?: preact.ComponentChildren; onParam: (key: string, v: number, final: boolean) => void; onPreset: (name: string) => void; onSavePreset?: () => void; automate?: (d: ParamDesc) => void }) {
  const mine = useStore(userPresets, (s) => s.list.filter((u) => u.plugin === p.state.id));
  const { info, state } = p;
  if (!info || p.missing)
    return (
      <div class="hint" style={{ padding: '12px', whiteSpace: 'normal' }}>
        Plugin <b>{state.id}</b> is not part of this firmware. Its settings are kept in the project and come back when a firmware with the plugin
        loads it.
      </div>
    );
  const groups: [string, ParamDesc[]][] = [];
  for (const d of info.params) {
    const g = groups.find((x) => x[0] === (d.group || 'Main'));
    if (g) g[1].push(d);
    else groups.push([d.group || 'Main', [d]]);
  }
  return (
    <div class="pe">
      {state.loadError ? <div class="hint" role="alert" style={{ padding: '8px', whiteSpace: 'normal' }}>
        Plugin could not load: {state.loadError}. Free memory by removing unused plugins, then reload the project.
      </div> : null}
      {p.resources}
      <div class="pe-head">
        <span class="hint">Preset</span>
        <select class="mini" value={state.preset ?? ''} aria-label="Preset" onChange={(e) => p.onPreset((e.target as HTMLSelectElement).value)}>
          <option value="">—</option>
          <optgroup label="Factory">
            {info.presets.map((pr) => (
              <option key={pr.name} value={pr.name}>
                {pr.category ? `${pr.category} · ` : ''}
                {pr.name}
              </option>
            ))}
          </optgroup>
          {mine.length ? (
            <optgroup label="Mine (on the device)">
              {mine.map((u) => (
                <option key={`u:${u.name}`} value={u.name}>
                  {u.name}
                </option>
              ))}
            </optgroup>
          ) : null}
        </select>
        {p.onSavePreset ? (
          <Btn class="icon" title="Save these settings as a preset on the device" onClick={p.onSavePreset}>
            <Icon name="save" />
          </Btn>
        ) : null}
        <span class="hint" title={info.description}>
          {info.name} {info.version} · {info.vendor} · {info.license}
          {info.capabilities.polyphonic ? ` · ${info.maxVoices} voices` : info.type === 'instrument' ? ' · mono' : ''}
        </span>
      </div>
      <div class="pe-groups">
        {groups.map(([name, ps]) => (
          <div class="pe-group" key={name}>
            <div class="pe-group-title">{name}</div>
            <div class="pe-ctls">
              {ps.map((d) => (
                <Param key={d.key} d={d} v={state.params[d.key] ?? d.def} onChange={(v, f) => p.onParam(d.key, v, f)} onAutomate={p.automate ? () => p.automate!(d) : undefined} />
              ))}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

export function EditorBody() {
  const ed = useStore(ui, (s) => s.editor);
  const project = useStore(proj, (s) => s.project);
  const plugins = useStore(catalog, (s) => s.plugins);
  if (!ed || !project) return <div class="hint" style={{ padding: '12px' }}>Click a channel name or an effect slot to edit it.</div>;
  if (ed.kind === 'channel') {
    const c = project.channels.find((x) => x.id === ed.id);
    if (!c) return null;
    const info = plugins.find((x) => x.id === c.plugin.id);
    return (
      <>
        <div class="row" style={{ marginBottom: '8px' }}>
          <span class="swatch" style={{ background: c.color, width: '14px', height: '14px' }} />
          <b>{c.name}</b>
          <Btn class="icon" title="Piano roll" onClick={() => openPiano(c.id)}>
            <Icon name="piano" />
          </Btn>
          <span class="hint">root key</span>
          <input type="number" min={0} max={127} value={c.root} style={{ width: '56px' }} aria-label="Root key" onChange={(e) => A.setChannel(c.id, { root: +(e.target as HTMLInputElement).value })} />
          <span class="hint">→ {c.track ? `Insert ${c.track}` : 'Master'}</span>
        </div>
        <PluginPanel
          info={info}
          state={c.plugin}
          missing={c.missing}
          resources={(info?.resourceSlots ?? []).map((s) => {
            const file = c.plugin.resources?.[s.key] ?? '';
            // a SoundFont slot on a plugin with bank / program parameters gets a preset menu
            const sf2 = s.file && /\bsf2\b/i.test(s.extensions) && info?.params.some((x) => x.key === 'bank') && info?.params.some((x) => x.key === 'program');
            return (
              <div key={s.key}>
                <ResourcePicker slot={s} value={file} missing={c.plugin.missingResources ?? []} onPick={(name) => A.setChannelResource(c.id, s.key, name)} />
                {sf2 && file && !(c.plugin.missingResources ?? []).includes(file) ? (
                  <Sf2PresetPicker
                    file={file}
                    bank={c.plugin.params.bank ?? 0}
                    program={c.plugin.params.program ?? 0}
                    onPick={(b, g) => {
                      A.setChannelParam(c.id, 'bank', b, true);
                      A.setChannelParam(c.id, 'program', g, true);
                    }}
                  />
                ) : null}
              </div>
            );
          })}
          onParam={(k, v, f) => A.setChannelParam(c.id, k, v, f)}
          onPreset={(n) => n && A.loadChannelPreset(c.id, n)}
          onSavePreset={() => A.saveUserPreset({ channel: c.id }, c.plugin.preset && !info?.presets.some((x) => x.name === c.plugin.preset) ? c.plugin.preset : `${c.name} preset`)}
          automate={(d) => A.automateParam({ kind: 'channelParam', channel: c.id, param: d.key }, `${c.name} ${d.name}`, normParam(d, c.plugin.params[d.key] ?? d.def))}
        />
      </>
    );
  }
  let fx = null as null | { f: (typeof project.mixer)[0]['fx'][0]; t: (typeof project.mixer)[0] };
  for (const t of project.mixer) for (const f of t.fx) if (f.id === ed.id) fx = { f, t };
  if (!fx) return null;
  const { f, t } = fx;
  const info = plugins.find((x) => x.id === f.plugin.id);
  return (
    <>
      <div class="row" style={{ marginBottom: '8px' }}>
        <b>{info?.name ?? f.plugin.id}</b>
        <span class="hint">on {t.name}</span>
        <Btn active={f.enabled} class="green" onClick={() => A.setFx(f.id, { enabled: !f.enabled })}>
          {f.enabled ? 'Enabled' : 'Bypassed'}
        </Btn>
        <Knob size={28} value={f.mix} min={0} max={1} def={1} label="Mix" onChange={(v, fin) => fin && A.setFx(f.id, { mix: v })} />
      </div>
      <PluginPanel
        info={info}
        state={f.plugin}
        missing={f.missing}
        onParam={(k, v, fin) => A.setFxParam(f.id, k, v, fin)}
        onPreset={(n) => n && A.loadFxPreset(f.id, n)}
        onSavePreset={() => A.saveUserPreset({ fx: f.id }, `${info?.name ?? 'Effect'} preset`)}
        automate={(d) => A.automateParam({ kind: 'fxParam', fx: f.id, param: d.key }, `${t.name} ${info?.name ?? ''} ${d.name}`, normParam(d, f.plugin.params[d.key] ?? d.def))}
      />
    </>
  );
}

/** a draggable floating window (desktop) */
export function FloatingEditor() {
  const open = useStore(ui, (s) => s.panels.plugin && !!s.editor);
  const title = useStore(proj, (s) => {
    const ed = ui.state.editor;
    if (!ed || !s.project) return '';
    if (ed.kind === 'channel') return s.project.channels.find((c) => c.id === ed.id)?.name ?? '';
    for (const t of s.project.mixer) for (const f of t.fx) if (f.id === ed.id) return `${t.name} · effect`;
    return '';
  });
  const [pos, setPos] = useState({ x: Math.max(20, window.innerWidth / 2 - 330), y: 90 });
  const drag = useRef<{ x: number; y: number } | null>(null);
  if (!open) return null;
  return (
    <div class="floating" style={{ left: `${pos.x}px`, top: `${pos.y}px`, width: 'min(680px, 96vw)' }}>
      <div
        class="modal-head"
        onPointerDown={(e) => {
          if (e.button !== 0 || (e.target as Element).closest('button, input, select, textarea, a, [role="button"]')) return;
          (e.currentTarget as Element).setPointerCapture(e.pointerId);
          drag.current = { x: e.clientX - pos.x, y: e.clientY - pos.y };
        }}
        onPointerMove={(e) => drag.current && setPos({ x: Math.max(0, e.clientX - drag.current.x), y: Math.max(0, e.clientY - drag.current.y) })}
        onPointerUp={() => (drag.current = null)}
        onPointerCancel={() => (drag.current = null)}
        onLostPointerCapture={() => (drag.current = null)}
      >
        <span>{title}</span>
        <button type="button" class="btn icon" aria-label="Close" onClick={() => ui.set((s) => ({ panels: { ...s.panels, plugin: false } }))}>
          <Icon name="x" />
        </button>
      </div>
      <div class="modal-body">
        <EditorBody />
      </div>
    </div>
  );
}
