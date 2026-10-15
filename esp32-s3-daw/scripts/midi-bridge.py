#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Computer MIDI -> DAW bridge (Linux, ALSA). Python standard library only.

Forwards a MIDI keyboard / controller / sequencer connected to a Linux computer to the DAW over
its WebSocket ("midi" messages, docs/PROTOCOL.md). Notes, CC, sustain, pitch bend, program change,
aftertouch, and MIDI clock / start / stop / continue (when the DAW follows external clock).
Use it when the browser has no Web MIDI (iOS Safari, Firefox without the permission, ...).

  scripts/midi-bridge.py --list
  scripts/midi-bridge.py --input hw:1,0,0 --host esp32daw.local
  scripts/midi-bridge.py --input seq:20:0 --host 192.168.4.1        (ALSA sequencer port, via aseqdump)
  scripts/midi-bridge.py --input - --host 127.0.0.1 --port 18190    (raw MIDI bytes from stdin)

Inputs:
  hw:CARD,DEV[,SUB] or /dev/snd/midiCxDy   a raw MIDI device (USB keyboards appear as one)
  seq:CLIENT:PORT                          an ALSA sequencer port (software synths' outputs, VMPK,
                                           a DAW on this computer); needs aseqdump (alsa-utils)
  -                                        raw MIDI bytes on stdin (testing, pipes)

By default MIDI channel n plays channel-rack row n+1; --channel ID sends everything to one DAW
channel (its id, shown in the channel's properties). The DAW releases this bridge's notes when the
connection drops; the bridge reconnects by itself.
"""
import argparse
import base64
import glob
import hashlib
import json
import os
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_BATCH = 768  # bytes per message (the DAW accepts up to 1024)


class Closed(Exception):
    pass


class DawLink:
    """A minimal RFC 6455 client: hello, then fire-and-forget "midi" messages. A reader thread
    drains the DAW's status broadcasts (an unread socket would make the DAW treat us as a slow
    client) and answers pings."""

    def __init__(self, host, port, name):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /ws HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise Closed("connection closed during the handshake")
            head += chunk
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise Closed("handshake refused: " + head.split(b"\r\n", 1)[0].decode(errors="replace"))
        want = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest())
        if not any(l.lower().startswith(b"sec-websocket-accept:") and l.split(b":", 1)[1].strip() == want for l in head.split(b"\r\n")):
            raise Closed("the server answered with a wrong Sec-WebSocket-Accept")
        self.buf = head.split(b"\r\n\r\n", 1)[1]
        self.lock = threading.Lock()
        self.alive = True
        self.welcome = threading.Event()
        self.project = ""
        self.send({"t": "hello", "proto": 1, "client": name})
        self.sock.settimeout(None)
        threading.Thread(target=self._reader, daemon=True).start()
        if not self.welcome.wait(30):
            self.close()
            raise Closed("no welcome from the DAW")

    def _frame(self, op, data):
        mask = os.urandom(4)
        n = len(data)
        hdr = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        return hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data))

    def send(self, msg, op=1):
        data = json.dumps(msg, separators=(",", ":")).encode() if op == 1 else msg
        with self.lock:
            try:
                self.sock.sendall(self._frame(op, data))
            except OSError as e:
                self.alive = False
                raise Closed(str(e))

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Closed("connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _reader(self):
        try:
            while self.alive:
                b0, b1 = self._need(2)
                n = b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._need(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._need(8))[0]
                payload = self._need(n)
                op = b0 & 0x0F
                if op == 8:
                    raise Closed("closed by the DAW")
                if op == 9:
                    self.send(payload, op=10)
                elif op == 1 and not self.welcome.is_set():
                    m = json.loads(payload)
                    if m.get("t") == "welcome":
                        self.project = (m.get("project") or {}).get("meta", {}).get("name", "")
                        self.welcome.set()
                    elif m.get("t") == "err":
                        print(f"midi-bridge: the DAW refused the hello: {m.get('msg')}", file=sys.stderr)
        except (Closed, OSError, ValueError):
            pass
        self.alive = False

    def close(self):
        self.alive = False
        try:
            self.sock.sendall(self._frame(8, b""))
        except OSError:
            pass
        self.sock.close()


# ---- inputs -----------------------------------------------------------------------------------

def raw_devices():
    out = []
    for path in sorted(glob.glob("/dev/snd/midiC*D*")):
        m = re.match(r".*/midiC(\d+)D(\d+)$", path)
        card, dev = int(m.group(1)), int(m.group(2))
        name = ""
        try:
            with open(f"/proc/asound/card{card}/id") as f:
                name = f.read().strip()
        except OSError:
            pass
        out.append((f"hw:{card},{dev}", path, name))
    return out


def seq_ports():
    try:
        txt = subprocess.run(["aconnect", "-i"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    out, client = [], None
    for line in txt.splitlines():
        m = re.match(r"client (\d+): '([^']*)'", line)
        if m:
            client = (m.group(1), m.group(2))
            continue
        m = re.match(r"\s+(\d+) '([^']*)'", line)
        if m and client and client[0] not in ("0", "14"):  # System, Midi Through
            out.append((f"seq:{client[0]}:{m.group(1)}", f"{client[1]} / {m.group(2).strip()}"))
    return out


class RawInput:
    def __init__(self, spec):
        if spec == "-":
            self.fd, self.close_fd = sys.stdin.buffer.fileno(), False
        else:
            path = spec
            m = re.match(r"hw:(\d+),(\d+)(?:,\d+)?$", spec)
            if m:
                path = f"/dev/snd/midiC{m.group(1)}D{m.group(2)}"
            self.fd, self.close_fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK), True

    def read(self, timeout):
        """bytes available within timeout ('' = none yet); EOFError when the device is gone"""
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        try:
            data = os.read(self.fd, 512)
        except BlockingIOError:
            return b""
        except OSError as e:
            raise EOFError(str(e))
        if not data:
            raise EOFError("end of input")
        return data

    def close(self):
        if self.close_fd:
            os.close(self.fd)


class SeqInput:
    """an ALSA sequencer port through aseqdump: its text events are turned back into MIDI bytes"""
    RX = re.compile(r"^\s*\d+:\d+\s+(.+?)\s{2,}(\d+)(?:,\s*(.*))?$")

    def __init__(self, spec):
        port = spec.split(":", 1)[1]
        self.p = subprocess.Popen(["aseqdump", "-p", port], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.fd = self.p.stdout.fileno()
        self.pending = ""

    @staticmethod
    def to_bytes(line):
        s = line.strip()
        for word, b in (("Clock", 0xF8), ("Start", 0xFA), ("Continue", 0xFB), ("Stop", 0xFC)):
            if re.match(rf"^\d+:\d+\s+{word}\s*$", s):
                return bytes([b])
        m = SeqInput.RX.match(line)
        if not m:
            return b""
        kind, ch = m.group(1).lower(), int(m.group(2)) & 15
        f = dict(re.findall(r"(\w+) (-?\d+)", m.group(3) or ""))
        v = lambda k: max(0, min(127, int(f.get(k, 0))))
        if kind == "note on":
            return bytes([0x90 | ch, v("note"), v("velocity")])
        if kind == "note off":
            return bytes([0x80 | ch, v("note"), v("velocity")])
        if kind == "control change":
            return bytes([0xB0 | ch, v("controller"), v("value")])
        if kind == "program change":
            return bytes([0xC0 | ch, v("program")])
        if kind == "channel aftertouch":
            return bytes([0xD0 | ch, v("value")])
        if kind == "polyphonic aftertouch":
            return bytes([0xA0 | ch, v("note"), v("value")])
        if kind == "pitch bend":
            b = max(0, min(16383, int(f.get("value", 0)) + 8192))
            return bytes([0xE0 | ch, b & 127, b >> 7])
        return b""

    def read(self, timeout):
        if self.p.poll() is not None:
            raise EOFError(self.p.stderr.read().strip() or "aseqdump exited")
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        chunk = os.read(self.fd, 4096).decode(errors="replace")
        if not chunk:
            raise EOFError("aseqdump exited")
        self.pending += chunk
        *lines, self.pending = self.pending.split("\n")
        return b"".join(self.to_bytes(l) for l in lines)

    def close(self):
        self.p.terminate()


def open_input(spec):
    return SeqInput(spec) if spec.startswith("seq:") else RawInput(spec)


# ---- main loop --------------------------------------------------------------------------------

def run(a):
    link, inp, backoff = None, None, 0.5
    sent = 0
    while True:
        try:
            if inp is None:
                inp = open_input(a.input)
                print(f"midi-bridge: reading {a.input}", file=sys.stderr)
            if link is None or not link.alive:
                link = DawLink(a.host, a.port, "midi-bridge")
                print(f"midi-bridge: connected to {a.host}:{a.port} (project '{link.project}')", file=sys.stderr)
                backoff = 0.5
            data = inp.read(0.25)
            # active sensing every 300 ms from many keyboards: not worth a network message
            data = bytes(b for b in data if b != 0xFE)
            if not data:
                continue
            ts = time.monotonic() * 1000.0
            for i in range(0, len(data), MAX_BATCH):
                msg = {"t": "midi", "data": list(data[i:i + MAX_BATCH]), "ts": round(ts, 3)}
                if a.channel:
                    msg["channel"] = a.channel
                link.send(msg)
            sent += len(data)
            if a.verbose:
                print(f"midi-bridge: {data.hex(' ')}", file=sys.stderr)
        except EOFError as e:
            if a.input == "-":
                print(f"midi-bridge: stdin closed after {sent} bytes", file=sys.stderr)
                time.sleep(a.linger)
                return 0
            print(f"midi-bridge: input lost ({e}); retrying", file=sys.stderr)
            if inp:
                inp.close()
            inp = None
            time.sleep(1.0)
        except (Closed, OSError) as e:
            if inp is None:
                print(f"midi-bridge: cannot open {a.input}: {e} (see --list; raw devices need the audio group)", file=sys.stderr)
                return 2
            print(f"midi-bridge: DAW connection: {e}; reconnecting in {backoff:.1f} s", file=sys.stderr)
            if link:
                link.alive = False
            link = None
            time.sleep(backoff)
            backoff = min(backoff * 2, 10.0)


def main():
    ap = argparse.ArgumentParser(description="Forward a Linux MIDI input to the ESP32-S3 DAW.")
    ap.add_argument("--list", action="store_true", help="list MIDI inputs")
    ap.add_argument("--input", help="hw:CARD,DEV | /dev/snd/midiCxDy | seq:CLIENT:PORT | -")
    ap.add_argument("--host", default="esp32daw.local", help="DAW address (default esp32daw.local)")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--channel", type=int, default=0, help="DAW channel id for everything (default: MIDI channel n -> rack row n+1)")
    ap.add_argument("--linger", type=float, default=0.5, help="with --input -: seconds to stay connected after stdin ends")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if a.list:
        raw, seq = raw_devices(), seq_ports()
        for spec, path, name in raw:
            print(f"{spec:14} {path:22} {name}")
        for spec, name in seq:
            print(f"{spec:14} {'(sequencer)':22} {name}")
        if not raw and not seq:
            print("no MIDI inputs found (is the keyboard plugged in? `amidi -l`, `aconnect -i`)")
        return 0
    if not a.input:
        ap.error("--input is required (see --list)")
    try:
        return run(a)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
