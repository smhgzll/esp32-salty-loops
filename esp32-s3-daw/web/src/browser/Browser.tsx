// SPDX-License-Identifier: GPL-3.0-or-later
// Browser panel: instruments and effects of this firmware, presets of the selected channel,
// the project's patterns, and the project slots on the device.
import { useState } from 'preact/hooks';

import * as A from '../app/actions';
import { catalog, proj, samples, slots, ui, userPresets } from '../app/state';
import { useStore } from '../app/store';
import { Btn, Icon } from '../ui/controls';
import { openProjectDialog } from '../settings/ProjectDialogs';
import { importMidi, isMidiFile } from '../midi/MidiImport';
import { importLibraryModule, importModule } from '../tracker/ModuleImport';
import { isModuleFile, MODULE_ACCEPT } from '../tracker/module';

type Tab = 'plugins' | 'samples' | 'presets' | 'patterns' | 'projects';
const TAB_NAME: Record<Tab, string> = { plugins: 'Plugins', samples: 'Samples', presets: 'Presets', patterns: 'Patterns', projects: 'Projects' };
const TAB_ICON: Record<Tab, string> = { plugins: 'plug', samples: 'wave', presets: 'save', patterns: 'steps', projects: 'folder' };

export function Browser() {
  const [tab, setTab] = useState<Tab>('plugins');
  const plugins = useStore(catalog, (s) => s.plugins);
  const project = useStore(proj, (s) => s.project);
  const selCh = useStore(ui, (s) => s.selectedChannel);
  const list = useStore(slots, (s) => s.list);
  const lib = useStore(samples, (s) => s.library);
  const mine = useStore(userPresets, (s) => s.list);
  const [drag, setDrag] = useState(false);
  const ch = project?.channels.find((c) => c.id === selCh);
  const chInfo = ch ? plugins.find((p) => p.id === ch.plugin.id) : undefined;
  const cats = (type: 'instrument' | 'effect') => {
    const m = new Map<string, typeof plugins>();
    for (const p of plugins.filter((x) => x.type === type)) m.set(p.category, [...(m.get(p.category) ?? []), p]);
    return [...m.entries()];
  };
  return (
    <div class="side">
      <div class="tabs" role="tablist">
        {(['plugins', 'samples', 'presets', 'patterns', 'projects'] as Tab[]).map((t) => (
          <button key={t} type="button" role="tab" class={`tab${tab === t ? ' on' : ''}`} aria-selected={tab === t} title={TAB_NAME[t]} onClick={() => setTab(t)}>
            <Icon name={TAB_ICON[t]} />
            {tab === t ? <span class="tab-label">{TAB_NAME[t]}</span> : null}
          </button>
        ))}
      </div>
      <div class="tree">
        {tab === 'plugins' ? (
          <>
            <div class="tree-group">Instruments — click to add a channel</div>
            {cats('instrument').map(([cat, ps]) => (
              <>
                {ps.map((p) => (
                  <div key={p.id} class="tree-item" title={p.description} onClick={() => A.addChannel(p.id)}>
                    <Icon name={cat === 'Drums' ? 'drum' : 'wave'} />
                    {p.name}
                    <span class="sub">{cat}</span>
                  </div>
                ))}
              </>
            ))}
            <div class="tree-group">Effects — click to insert on the selected mixer track</div>
            {cats('effect').map(([cat, ps]) => (
              <>
                {ps.map((p) => (
                  <div key={p.id} class="tree-item" title={p.description} onClick={() => A.addFx(ui.state.mixerTrack, p.id)}>
                    <Icon name="plug" />
                    {p.name}
                    <span class="sub">{cat}</span>
                  </div>
                ))}
              </>
            ))}
            <div class="tree-group">{plugins.length} plugins linked in this firmware</div>
            <div class="hint" style={{ padding: '0 10px', whiteSpace: 'normal' }}>
              New plugins are added at firmware build time (esp32-daw-plugin-sdk packages); uploading native code at run time is not supported.
            </div>
          </>
        ) : null}
        {tab === 'samples' ? (
          <div
            class={`drop${drag ? ' over' : ''}`}
            onDragOver={(e) => {
              e.preventDefault();
              setDrag(true);
            }}
            onDragLeave={() => setDrag(false)}
            onDrop={(e) => {
              e.preventDefault();
              setDrag(false);
              const files = [...(e.dataTransfer?.files ?? [])];
              // MIDI files are not samples: they open the MIDI import; a tracker module opens its import
              const midi = files.find(isMidiFile);
              if (midi) importMidi('project', midi);
              const mod = files.find(isModuleFile);
              if (mod && !midi) importModule(mod);
              const rest = files.filter((f) => !isMidiFile(f) && !isModuleFile(f));
              if (rest.length) A.uploadSamples(rest);
            }}
          >
            <div class="tree-group">Sample library on the device — click to make a Sampler channel (a module: import it)</div>
            {lib.list.map((f) => (
              <div
                key={f.name}
                class="tree-item sample-item"
                title={`${f.name} · ${Math.round(f.size / 1024)} KB`}
                onClick={() => (f.type === 'module' ? importLibraryModule(f.name) : A.addSampleChannel(f.name))}
              >
                <Icon name={f.type === 'sf2' ? 'piano' : f.type === 'module' ? 'steps' : 'wave'} />
                <span class="grow">{f.name}</span>
                <span class="sub">{f.size >= 1048576 ? `${(f.size / 1048576).toFixed(1)} MB` : `${Math.round(f.size / 1024)} KB`}</span>
                <button
                  type="button"
                  class="btn icon flat"
                  title={`Delete ${f.name} from the device`}
                  aria-label={`Delete ${f.name}`}
                  onClick={(e) => {
                    e.stopPropagation();
                    if (confirm(`Delete "${f.name}" from the device? Channels using it go silent.`)) A.deleteSample(f.name);
                  }}
                >
                  <Icon name="x" />
                </button>
              </div>
            ))}
            {!lib.list.length ? <div class="hint" style={{ padding: '4px 10px' }}>No samples yet.</div> : null}
            <div style={{ padding: '6px 10px' }}>
              <label class="btn" title="WAV (8 / 16 / 24 / 32-bit, float), SoundFont .sf2 or a tracker module (MOD, XM, S3M, IT); or drop files here">
                <Icon name="plus" /> Upload…
                <input
                  type="file"
                  accept={`.wav,.sf2,${MODULE_ACCEPT},audio/wav,audio/x-wav`}
                  multiple
                  aria-label="Upload samples"
                  style={{ display: 'none' }}
                  onChange={(e) => {
                    const fl = (e.target as HTMLInputElement).files;
                    if (fl?.length) A.uploadSamples(fl);
                    (e.target as HTMLInputElement).value = '';
                  }}
                />
              </label>
            </div>
            {lib.total ? (
              <div class="hint" style={{ padding: '0 10px', whiteSpace: 'normal' }}>
                {Math.round((lib.total - (lib.free ?? 0)) / 1024)} KB used of {Math.round(lib.total / 1024)} KB · largest file that fits: {Math.round((lib.largest ?? 0) / 1024)} KB.
                Uploads are refused while playing (flash writes would interrupt the audio).
              </div>
            ) : (
              <div class="hint" style={{ padding: '0 10px' }}>This target has no sample library.</div>
            )}
          </div>
        ) : null}
        {tab === 'presets' ? (
          ch && chInfo ? (
            <>
              <div class="tree-group">
                {ch.name} · {chInfo.name}
              </div>
              {chInfo.presets.map((pr) => (
                <div key={pr.name} class={`tree-item${ch.plugin.preset === pr.name ? ' on' : ''}`} onClick={() => A.loadChannelPreset(ch.id, pr.name)}>
                  {pr.name}
                  <span class="sub">{pr.category}</span>
                </div>
              ))}
              <div class="tree-group">Mine — saved on the device</div>
              {mine
                .filter((u) => u.plugin === chInfo.id)
                .map((u) => (
                  <div key={u.name} class={`tree-item sample-item${ch.plugin.preset === u.name ? ' on' : ''}`} onClick={() => A.loadChannelPreset(ch.id, u.name)}>
                    <Icon name="save" />
                    <span class="grow">{u.name}</span>
                    <button
                      type="button"
                      class="btn icon flat"
                      aria-label={`Delete preset ${u.name}`}
                      title={`Delete the preset ${u.name} from the device`}
                      onClick={(e) => {
                        e.stopPropagation();
                        if (confirm(`Delete the preset "${u.name}"?`)) A.deleteUserPreset(u.plugin, u.name);
                      }}
                    >
                      <Icon name="x" />
                    </button>
                  </div>
                ))}
              <div style={{ padding: '6px 10px' }}>
                <Btn onClick={() => A.saveUserPreset({ channel: ch.id }, `${ch.name} preset`)}>
                  <Icon name="save" /> Save current…
                </Btn>
              </div>
            </>
          ) : (
            <div class="hint" style={{ padding: '10px' }}>Select a channel.</div>
          )
        ) : null}
        {tab === 'patterns' && project ? (
          <>
            {project.patterns.map((p) => (
              <div key={p.id} class={`tree-item${project.meta.currentPattern === p.id ? ' on' : ''}`} onClick={() => A.selectPattern(p.id)}>
                <span class="swatch" style={{ background: p.color }} />
                {p.name}
                <span class="sub">{Math.round((p.effLength || 3840) / 3840)} bar</span>
              </div>
            ))}
            <div style={{ padding: '6px 10px' }}>
              <Btn onClick={() => A.addPattern().then((id) => A.selectPattern(id, false))}>
                <Icon name="plus" /> Pattern
              </Btn>
            </div>
          </>
        ) : null}
        {tab === 'projects' ? (
          <>
            {list.map((s) => (
              <div key={s.slot} class="tree-item" onClick={() => s.used && confirm(`Load "${s.name}"? Unsaved changes stay in the autosave only.`) && A.loadProject(s.slot)}>
                <Icon name={s.autosave ? 'save' : 'folder'} />
                {s.used ? s.name : <span class="hint">empty</span>}
                <span class="sub">{s.autosave ? 'autosave' : `slot ${s.slot}`}</span>
              </div>
            ))}
            <div class="row" style={{ padding: '6px 10px' }}>
              <Btn onClick={() => openProjectDialog('save')}>
                <Icon name="save" /> Save as…
              </Btn>
            </div>
          </>
        ) : null}
      </div>
    </div>
  );
}
