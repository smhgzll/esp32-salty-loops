// SPDX-License-Identifier: GPL-3.0-or-later
// Device settings (admin login): Wi-Fi mode, network name and password, device name, admin
// password. Passwords are write-only: the device never sends them back; leave a field empty to keep
// it. Wi-Fi and the device name apply after a restart; a new admin password applies at once.
import { useEffect, useState } from 'preact/hooks';

import { authed, authedGet } from '../app/protocol';
import { toast } from '../app/state';
import { Btn, Modal } from '../ui/controls';
import { ensureLogin } from './ProjectDialogs';

interface Current {
  wifiMode: 'AP' | 'STA';
  ssid: string;
  hostname: string;
  wifiPasswordSet: boolean;
  adminPasswordSet: boolean;
}

export function DeviceSettingsDialog(p: { onClose: () => void }) {
  const [cur, setCur] = useState<Current | null>(null);
  const [mode, setMode] = useState<'AP' | 'STA'>('AP');
  const [ssid, setSsid] = useState('');
  const [host, setHost] = useState('');
  const [wifiPw, setWifiPw] = useState('');
  const [clearWifiPw, setClearWifiPw] = useState(false);
  const [admin, setAdmin] = useState('');
  const [busy, setBusy] = useState(false);
  const [saved, setSaved] = useState(false);
  useEffect(() => {
    (async () => {
      if (!(await ensureLogin())) return p.onClose();
      try {
        const c = (await authedGet('/api/settings')) as Current;
        setCur(c);
        setMode(c.wifiMode);
        setSsid(c.ssid);
        setHost(c.hostname);
      } catch (e) {
        toast((e as Error).message, 'error');
        p.onClose();
      }
    })();
  }, []);
  const save = async () => {
    if (!cur) return;
    const body: Record<string, string> = {};
    if (mode !== cur.wifiMode) body.wifiMode = mode;
    if (ssid !== cur.ssid) body.ssid = ssid;
    if (host !== cur.hostname) body.hostname = host;
    if (clearWifiPw) body.wifiPassword = '';
    else if (wifiPw) body.wifiPassword = wifiPw;
    if (admin) body.adminPassword = admin;
    if (!Object.keys(body).length) return p.onClose();
    setBusy(true);
    try {
      await authed('/api/settings', JSON.stringify(body));
      setSaved(true);
      toast(admin ? 'Saved. The new admin password is in effect: log in again for privileged operations' : 'Saved');
    } catch (e) {
      toast((e as Error).message, 'error');
    } finally {
      setBusy(false);
    }
  };
  const restart = async () => {
    try {
      await authed('/api/reboot', '{}');
      toast('Restarting: reconnect to the device with the new settings');
      p.onClose();
    } catch (e) {
      toast((e as Error).message, 'error');
    }
  };
  return (
    <Modal title="Device settings" onClose={p.onClose}>
      {!cur ? (
        <div class="hint">Loading…</div>
      ) : (
        <form
          class="form"
          onSubmit={(e) => {
            e.preventDefault();
            save();
          }}
        >
          <label>
            Wi-Fi
            <select value={mode} aria-label="Wi-Fi mode" onChange={(e) => setMode((e.target as HTMLSelectElement).value as 'AP' | 'STA')}>
              <option value="AP">its own access point</option>
              <option value="STA">join a network</option>
            </select>
          </label>
          <label>
            {mode === 'AP' ? 'Access point name' : 'Network name'}
            <input value={ssid} maxLength={32} aria-label="Network name" onInput={(e) => setSsid((e.target as HTMLInputElement).value)} />
          </label>
          <label>
            Wi-Fi password
            <input
              type="password"
              value={wifiPw}
              disabled={clearWifiPw}
              minLength={8}
              maxLength={63}
              placeholder={cur.wifiPasswordSet ? 'unchanged' : mode === 'AP' ? 'none: a random one at every boot' : 'none (open network)'}
              aria-label="Wi-Fi password"
              onInput={(e) => setWifiPw((e.target as HTMLInputElement).value)}
            />
          </label>
          {cur.wifiPasswordSet ? (
            <label class="check">
              <input type="checkbox" checked={clearWifiPw} onChange={(e) => setClearWifiPw((e.target as HTMLInputElement).checked)} /> remove the Wi-Fi password
            </label>
          ) : null}
          <label>
            Device name
            <input value={host} maxLength={32} pattern="[a-z0-9]([a-z0-9-]*[a-z0-9])?" aria-label="Device name" onInput={(e) => setHost((e.target as HTMLInputElement).value)} />
          </label>
          <label>
            New admin password
            <input type="password" value={admin} minLength={8} maxLength={64} placeholder="unchanged" aria-label="New admin password" onInput={(e) => setAdmin((e.target as HTMLInputElement).value)} />
          </label>
          <div class="hint" style={{ whiteSpace: 'normal' }}>
            Wi-Fi and the device name take effect after a restart. If the device cannot join the network, it opens its own access point again.
          </div>
          <div class="row" style={{ justifyContent: 'flex-end', gap: '6px' }}>
            {saved ? <Btn onClick={restart}>Restart now</Btn> : null}
            <Btn onClick={p.onClose}>{saved ? 'Close' : 'Cancel'}</Btn>
            <button type="submit" class="btn green on" disabled={busy}>
              Save
            </button>
          </div>
        </form>
      )}
    </Modal>
  );
}
