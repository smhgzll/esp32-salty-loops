// SPDX-License-Identifier: GPL-3.0-or-later
// Device & diagnostics: engine load, per-plugin CPU / voices / memory, memory and audio counters,
// diagnostic tones, firmware update, factory reset, MIDI input from the browser.
import { useEffect, useState } from 'preact/hooks';

import * as A from '../app/actions';
import { authed } from '../app/protocol';
import { conn, live, proj, toast, ui } from '../app/state';
import { useStore } from '../app/store';
import { Btn, Icon } from '../ui/controls';
import { togglePanel } from '../transport/TopBar';
import { ensureLogin, exportProject, importProjectFile } from './ProjectDialogs';
import { midiInputs, startMidi } from './midi';
import { DeviceSettingsDialog } from './DeviceSettings';

const kb = (b: unknown) => (typeof b === 'number' ? `${Math.round(b / 1024)} KB` : '—');

export function Settings(p: { mobile?: boolean }) {
  const d = useStore(live, (s) => s.diag);
  const server = useStore(conn, (s) => s.server);
  const project = useStore(proj, (s) => s.project);
  const [ota, setOta] = useState('');
  const [devSettings, setDevSettings] = useState(false);
  const [inputs, setInputs] = useState<string[]>([]);
  useEffect(() => {
    midiInputs().then(setInputs);
  }, []);
  const name = (where: string, id: number) =>
    where === 'channel' ? project?.channels.find((c) => c.id === id)?.name : project?.mixer.find((t) => t.fx.some((f) => f.id === id))?.name + ' fx';
  const upload = async (file: File) => {
    if (!(await ensureLogin())) return;
    setOta(`uploading ${file.name} (${Math.round(file.size / 1024)} KB)… audio pauses during the update`);
    try {
      await authed('/api/ota', file, 'application/octet-stream');
      setOta('written and verified: the device restarts into the new firmware and confirms it after a healthy start');
    } catch (e) {
      setOta(`refused: ${(e as Error).message}`);
    }
  };
  return (
    <div class="panel" style={{ flex: 1 }}>
      <div class="panel-head">
        <span class="panel-title">Device &amp; diagnostics</span>
        <div class="spacer" />
        {!p.mobile ? (
          <Btn class="icon flat" title="Close" onClick={() => togglePanel('settings')}>
            <Icon name="x" />
          </Btn>
        ) : null}
      </div>
      <div class="panel-body" style={{ overflow: 'auto', padding: '8px 12px' }}>
        <h3>Device</h3>
        <dl class="kv">
          <dt>Device</dt>
          <dd>
            {server?.name} ({server?.target}, firmware {server?.version})
          </dd>
          <dt>Audio</dt>
          <dd>
            {server?.sampleRate} Hz, {server?.blockFrames}-frame blocks ({(((server?.blockFrames ?? 256) / (server?.sampleRate ?? 44100)) * 1000).toFixed(2)} ms), backend{' '}
            {String(d?.audioBackend ?? '—')}
          </dd>
          <dt>Processor</dt>
          <dd>{typeof d?.cpuMHz === 'number' ? `ESP32-S3 · ${d.cpuMHz} MHz` : '—'}</dd>
          <dt>DSP load</dt>
          <dd>
            {d ? `${d.cpu.toFixed(1)} % (render ${d.renderUsAvg} µs avg, ${d.renderUsMax} µs max of ${d.budgetUs} µs; now ${(d.load ?? 0).toFixed(0)} %)` : '—'}
          </dd>
          <dt>Problems</dt>
          <dd>
            {d
              ? `overruns ${d.overruns}, underruns ${d.underruns ?? '—'}, event drops ${d.eventDrops}, note drops ${d.noteDrops}, CPU-guard skips ${d.cpuGuardDrops ?? 0}, NaN resets ${d.nanResets}, WS frames dropped ${d.wsDropped ?? 0}`
              : '—'}
          </dd>
          <dt>Memory</dt>
          <dd>{d ? `internal ${kb(d.heapInternal)} free (min ${kb(d.heapInternalMin)}, largest block ${kb(d.heapInternalLargest)}), PSRAM ${kb(d.psramFree)} free of ${kb(d.psramTotal)}` : '—'}</dd>
          <dt>Uptime</dt>
          <dd>{d?.uptimeMs ? `${Math.round((d.uptimeMs as number) / 1000)} s, boot #${d.boot}, partition ${d.partition}` : '—'}</dd>
        </dl>
        <p class="hint" style={{ whiteSpace: 'normal' }}>
          The load figures come from the device itself. Under QEMU they reflect the emulator, not the ESP32-S3: only hardware measurements count.
        </p>
        <h3>Plugins</h3>
        <table class="grid">
          <thead>
            <tr>
              <th>Where</th>
              <th>Plugin</th>
              <th>CPU %</th>
              <th>Voices</th>
              <th>Fast RAM</th>
              <th>PSRAM</th>
            </tr>
          </thead>
          <tbody>
            {(d?.plugins ?? []).map((x) => (
              <tr key={`${x.where}${x.id}`}>
                <td>{name(x.where, x.id)}</td>
                <td>{x.plugin.replace('org.smhgzll.', '')}</td>
                <td>{x.cpu.toFixed(1)}</td>
                <td>{x.voices}</td>
                <td>{kb(x.memFast)}</td>
                <td>{kb(x.memLarge)}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <h3>Test signals</h3>
        <div class="row">
          {[
            ['sine440', '440 Hz sine'],
            ['leftright', 'Left / right'],
            ['lowlevel', '-60 dBFS'],
            ['silence', 'Silence'],
            ['click', 'Click'],
            ['off', 'Off'],
          ].map(([k, l]) => (
            <Btn key={k} onClick={() => A.diagTone(k)}>
              {l}
            </Btn>
          ))}
          <Btn class="danger" onClick={A.panic}>
            Panic (all sound off)
          </Btn>
        </div>
        <h3>MIDI input (this browser)</h3>
        <div class="row">
          {inputs.length ? (
            <>
              <span class="hint">{inputs.join(', ')}</span>
              <Btn onClick={() => startMidi().then(() => toast('MIDI input routed to the selected channel'))}>Connect</Btn>
            </>
          ) : (
            <span class="hint" style={{ whiteSpace: 'normal' }}>
              Web MIDI is not available in this browser (iOS Safari has none). Use the on-screen keyboard, or the ALSA bridge on a Linux computer
              (scripts/midi-bridge.py).
            </span>
          )}
        </div>
        <h3>Project</h3>
        <div class="row">
          <Btn onClick={exportProject}>Export to file</Btn>
          <Btn onClick={importProjectFile}>Import from file…</Btn>
        </div>
        <h3>Firmware</h3>
        <div class="row">
          <input type="file" accept=".bin" aria-label="Firmware image" onChange={(e) => {
            const f = (e.target as HTMLInputElement).files?.[0];
            if (f) upload(f);
          }} />
          <span class="hint" style={{ whiteSpace: 'normal' }}>{ota || 'build/esp32_s3_daw.bin of a matching profile; projects are kept'}</span>
        </div>
        <div class="row">
          <a class="hint" href="/licenses.txt" target="_blank" rel="noopener">
            Licenses and third-party notices
          </a>
        </div>
        <h3>Network and access</h3>
        <div class="row">
          <Btn onClick={() => setDevSettings(true)}>Device settings…</Btn>
          <span class="hint" style={{ whiteSpace: 'normal' }}>Wi-Fi, device name, admin password (needs the admin login)</span>
        </div>
        {devSettings ? <DeviceSettingsDialog onClose={() => setDevSettings(false)} /> : null}
        <h3>Danger zone</h3>
        <div class="row">
          <Btn
            class="danger"
            onClick={async () => {
              if (!confirm('Erase every stored project and the device settings, then restart?')) return;
              if (!(await ensureLogin())) return;
              try {
                await authed('/api/factory-reset', '{}');
                toast('Factory reset: restarting');
              } catch (e) {
                toast((e as Error).message, 'error');
              }
            }}
          >
            Factory reset
          </Btn>
          <Btn onClick={() => ui.set({ token: '' })}>Log out</Btn>
        </div>
      </div>
    </div>
  );
}
