// SPDX-License-Identifier: GPL-3.0-or-later
// Web MIDI input (where the browser has it): note / CC / clock bytes go to the device as "midi"
// messages with the browser's timestamps, targeted at the selected channel. Clock ticks are sent with
// performance-time stamps so the device follows the sender's timing, not WebSocket arrival times.
import { send } from '../app/protocol';
import { ui } from '../app/state';

let access: any = null;

export async function midiInputs(): Promise<string[]> {
  const nav = navigator as any;
  if (!nav.requestMIDIAccess) return [];
  try {
    access = access ?? (await nav.requestMIDIAccess({ sysex: false }));
    return [...access.inputs.values()].map((i: any) => i.name);
  } catch {
    return [];
  }
}

export async function startMidi() {
  if (!access) await midiInputs();
  if (!access) return;
  const origin = Date.now() - performance.now();
  for (const input of access.inputs.values())
    input.onmidimessage = (e: any) => {
      const data = [...(e.data as Uint8Array)];
      if (data[0] >= 0xf8 && data[0] !== 0xf8 && data[0] !== 0xfa && data[0] !== 0xfb && data[0] !== 0xfc) return;
      send('midi', { data, channel: data[0] < 0xf0 ? ui.state.selectedChannel : 0, ts: origin + e.timeStamp });
    };
}
