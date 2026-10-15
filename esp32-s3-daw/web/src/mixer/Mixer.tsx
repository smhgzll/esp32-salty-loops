// SPDX-License-Identifier: GPL-3.0-or-later
// Mixer: master + 16 insert strips. Each strip: name, peak meters, fader, pan, mute / solo, route,
// insert effects (4 slots). The selected strip shows its sends and full effect chain.
import * as A from '../app/actions';
import { catalog, live, proj, ui } from '../app/state';
import { useStore } from '../app/store';
import type { Track } from '../app/types';
import { dbText, PALETTE } from '../app/util';
import { Btn, Fader, Icon, Knob, Meter, menuAt, showMenu } from '../ui/controls';
import type { MenuItem } from '../ui/controls';
import { togglePanel } from '../transport/TopBar';

export function effectMenu(onPick: (id: string) => void): MenuItem[] {
  return catalog.state.plugins.filter((p) => p.type === 'effect').map((p) => ({ label: `${p.name}  ·  ${p.category}`, action: () => onPick(p.id) }));
}

export function openFx(id: number) {
  ui.set((s) => ({ editor: { kind: 'fx', id }, panels: { ...s.panels, plugin: true } }));
}

function StripMeter(p: { index: number; h: number }) {
  const v = useStore(live, (s) => [s.status?.m[p.index * 2] ?? 0, s.status?.m[p.index * 2 + 1] ?? 0]);
  return <Meter l={v[0]} r={v[1]} height={p.h} width={5} />;
}

function Strip(p: { t: Track; sel: boolean; h: number }) {
  const { t } = p;
  const fxMenu = (e: MouseEvent, i: number) => {
    const f = t.fx[i];
    if (!f) {
      menuAt(e, effectMenu((id) => A.addFx(t.index, id)));
      return;
    }
    menuAt(e, [
      { label: 'Open', action: () => openFx(f.id) },
      { label: f.enabled ? 'Bypass' : 'Enable', action: () => A.setFx(f.id, { enabled: !f.enabled }) },
      { label: 'Move up', disabled: i === 0, action: () => A.moveFx(f.id, i - 1) },
      { label: 'Move down', disabled: i >= t.fx.length - 1, action: () => A.moveFx(f.id, i + 1) },
      { label: 'Remove', danger: true, action: () => A.removeFx(f.id) },
    ]);
  };
  const faderH = Math.max(70, p.h - 230);
  return (
    <div class={`strip${t.index === 0 ? ' master' : ''}${p.sel ? ' sel' : ''}`} onPointerDown={() => ui.set({ mixerTrack: t.index })}>
      <div
        class="strip-name"
        style={{ background: t.color }}
        title={`${t.name}: right-click to rename / color`}
        onContextMenu={(e) => {
          e.preventDefault();
          showMenu(
            [
              { label: 'Rename…', action: () => { const n = prompt('Track name', t.name); if (n) A.setTrack(t.index, { name: n }); } },
              ...PALETTE.slice(0, 10).map((c) => ({ label: `Color ${c}`, action: () => A.setTrack(t.index, { color: c }) })),
            ],
            e.clientX,
            e.clientY,
          );
        }}
        onDblClick={() => { const n = prompt('Track name', t.name); if (n) A.setTrack(t.index, { name: n }); }}
      >
        {t.name}
      </div>
      <span class="strip-num">{t.index === 0 ? 'M' : t.index}</span>
      <div class="strip-fx">
        {[0, 1, 2, 3].map((i) => {
          const f = t.fx[i];
          const name = f ? catalog.state.plugins.find((x) => x.id === f.plugin.id)?.name ?? (f.missing ? `? ${f.plugin.id}` : f.plugin.id) : '';
          return (
            <div
              key={i}
              class={`fx-slot ${f ? (f.enabled ? 'on' : 'off') : 'empty'}`}
              title={f ? `${name}: click to open, right-click for options` : 'Add an effect'}
              onClick={(e) => (f ? openFx(f.id) : fxMenu(e as MouseEvent, i))}
              onContextMenu={(e) => {
                e.preventDefault();
                fxMenu(e as MouseEvent, i);
              }}
            >
              {f ? '●' : '+'} {name}
            </div>
          );
        })}
      </div>
      <Knob size={26} value={t.pan} min={-1} max={1} def={0} scale="bipolar" title={`Pan ${Math.round(t.pan * 100)}`} onChange={(v, f) => A.setTrack(t.index, { pan: v }, f)} />
      <div class="strip-fader">
        <StripMeter index={t.index} h={faderH} />
        <Fader value={t.vol} height={faderH} def={0.8} color={t.color} onChange={(v, f) => A.setTrack(t.index, { vol: v }, f)} />
      </div>
      <span class="strip-db">{dbText(t.vol)} dB</span>
      <div class="ms">
        <Btn active={!t.mute} class="green" title={t.mute ? 'Unmute' : 'Mute'} onClick={() => A.setTrack(t.index, { mute: !t.mute })}>
          {t.mute ? 'M' : 'ON'}
        </Btn>
        {t.index ? (
          <Btn active={t.solo} title="Solo" onClick={() => A.setTrack(t.index, { solo: !t.solo })}>
            S
          </Btn>
        ) : null}
      </div>
      {t.index ? (
        <select class="route mini" value={t.route} title="Output" aria-label="Output" onChange={(e) => A.setTrack(t.index, { route: +(e.target as HTMLSelectElement).value })}>
          <option value={0}>→ Master</option>
          {Array.from({ length: 16 }, (_, i) => i + 1)
            .filter((i) => i !== t.index)
            .map((i) => (
              <option key={i} value={i}>
                → {i}
              </option>
            ))}
        </select>
      ) : (
        <span class="hint">output</span>
      )}
    </div>
  );
}

function Sends(p: { t: Track }) {
  const { t } = p;
  if (t.index === 0) return <div class="hint" style={{ padding: '6px' }}>The master track has no sends.</div>;
  const slots = [0, 1, 2, 3].map((i) => t.sends[i] ?? { target: 0, level: 0 });
  return (
    <div class="row" style={{ padding: '4px 8px', gap: '12px' }}>
      <span class="hint">Sends of {t.name}:</span>
      {slots.map((s, i) => (
        <div key={i} class="row" style={{ gap: '4px' }}>
          <select class="mini" value={s.target} aria-label={`Send ${i + 1} target`} onChange={(e) => A.setSend(t.index, i, +(e.target as HTMLSelectElement).value, s.level)}>
            <option value={0}>Master</option>
            {Array.from({ length: 16 }, (_, k) => k + 1)
              .filter((k) => k !== t.index)
              .map((k) => (
                <option key={k} value={k}>
                  Insert {k}
                </option>
              ))}
          </select>
          <Knob size={24} value={s.level} min={0} max={1.25} def={0} title={`Send ${i + 1}: ${Math.round(s.level * 100)} %`} onChange={(v, f) => A.setSend(t.index, i, s.target, v, f)} />
        </div>
      ))}
    </div>
  );
}

export function Mixer(p: { mobile?: boolean; height?: number }) {
  const mixer = useStore(proj, (s) => s.project?.mixer);
  const sel = useStore(ui, (s) => s.mixerTrack);
  if (!mixer) return null;
  const h = p.height ?? 330;
  const cur = mixer.find((t) => t.index === sel) ?? mixer[0];
  return (
    <div class="panel" style={{ flex: p.mobile ? 1 : 'none', height: p.mobile ? undefined : `${h}px` }}>
      <div class="panel-head">
        <span class="panel-title">Mixer</span>
        <span class="hint">{cur.name}</span>
        <div class="spacer" />
        <Btn class="icon" title="Add an effect to the selected track" onClick={(e) => menuAt(e, effectMenu((id) => A.addFx(cur.index, id)))}>
          <Icon name="plus" /> FX
        </Btn>
        {!p.mobile ? (
          <Btn class="icon flat" title="Close" onClick={() => togglePanel('mixer')}>
            <Icon name="x" />
          </Btn>
        ) : null}
      </div>
      <div class="panel-body" style={{ display: 'flex', flexDirection: 'column' }}>
        <div class="mixer" style={{ flex: 1 }}>
          {[mixer[0], ...mixer.slice(1)].map((t) => (
            <Strip key={t.index} t={t} sel={t.index === sel} h={p.mobile ? h - 40 : h - 60} />
          ))}
        </div>
        <Sends t={cur} />
      </div>
    </div>
  );
}
