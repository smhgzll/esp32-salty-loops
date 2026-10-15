// SPDX-License-Identifier: GPL-3.0-or-later
// Project slots (open / save as), export to a file, import from a file (privileged: needs a login).
import { render } from 'preact';
import { useState } from 'preact/hooks';

import * as A from '../app/actions';
import { authed, login } from '../app/protocol';
import { conn, P, slots, toast, ui } from '../app/state';
import { useStore } from '../app/store';
import { Btn, Modal } from '../ui/controls';

let host: HTMLElement | null = null;
function mount(node: preact.ComponentChild) {
  if (!host) {
    host = document.createElement('div');
    document.body.appendChild(host);
  }
  render(node, host);
}
const close = () => host && render(null, host);

function SlotDialog(p: { mode: 'open' | 'save' }) {
  const list = useStore(slots, (s) => s.list);
  const limits = useStore(conn, (s) => s.server?.limits);
  const [name, setName] = useState(P()?.meta.name ?? 'Untitled');
  return (
    <Modal title={p.mode === 'open' ? 'Open project' : 'Save project as'} onClose={close}>
      {p.mode === 'save' ? (
        <div class="field">
          <label>Name</label>
          <input value={name} maxLength={64} onInput={(e) => setName((e.target as HTMLInputElement).value)} />
        </div>
      ) : null}
      <table class="grid">
        <thead>
          <tr>
            <th>Slot</th>
            <th>Project</th>
            <th>Size</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {list.map((s) => (
            <tr key={s.slot}>
              <td>{s.autosave ? 'auto' : s.slot}</td>
              <td>{s.used ? s.name : <span class="hint">empty</span>}</td>
              <td>{s.used ? `${Math.round((s.bytes ?? 0) / 1024)} KB` : ''}</td>
              <td>
                {p.mode === 'open' ? (
                  <Btn disabled={!s.used} onClick={() => { A.loadProject(s.slot); close(); }}>Open</Btn>
                ) : (
                  <Btn disabled={s.autosave} onClick={() => { if (!s.used || confirm(`Overwrite "${s.name}"?`)) { A.saveProject(s.slot, name); close(); } }}>Save here</Btn>
                )}
                {!s.autosave && s.used && p.mode === 'open' ? (
                  <Btn class="danger" onClick={() => confirm(`Delete "${s.name}"?`) && A.deleteProject(s.slot)}>Delete</Btn>
                ) : null}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <p class="hint" style={{ whiteSpace: 'normal' }}>
        {limits?.projectSlots ?? 7} slots of up to {Math.round((limits?.projectBytes ?? 0) / 1024)} KB in the device's flash
        {limits?.projectDocBytes ? ' (projects are stored compressed: a few times larger ones fit)' : ''}. The working project is autosaved while the
        transport is stopped.
      </p>
    </Modal>
  );
}

export const openProjectDialog = (mode: 'open' | 'save') => mount(<SlotDialog mode={mode} />);

export function exportProject() {
  const a = document.createElement('a');
  a.href = '/api/project';
  a.download = `${(P()?.meta.name ?? 'project').replace(/[^\w.-]+/g, '_')}.esp32daw.json`;
  document.body.appendChild(a);
  a.click();
  a.remove();
}

export async function ensureLogin(): Promise<boolean> {
  if (ui.state.token) return true;
  const pw = prompt('Device admin password (privileged operation)');
  if (!pw) return false;
  try {
    await login(pw);
    return true;
  } catch (e) {
    toast((e as Error).message, 'error');
    return false;
  }
}

export function importProjectFile() {
  const inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = '.json,application/json';
  inp.onchange = async () => {
    const f = inp.files?.[0];
    if (!f) return;
    if (!(await ensureLogin())) return;
    try {
      await authed('/api/project', await f.text());
      toast(`Imported ${f.name}`);
    } catch (e) {
      toast(`Import failed: ${(e as Error).message}`, 'error');
    }
  };
  inp.click();
}
