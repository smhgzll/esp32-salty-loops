# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal blocking WebSocket client and HTTP helpers (Python standard library only)."""
import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import time


class WSError(Exception):
    pass


class WS:
    def __init__(self, host, port, origin=None, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        key = base64.b64encode(os.urandom(16)).decode()
        req = f"GET /ws HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" \
              f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
        if origin:
            req += f"Origin: {origin}\r\n"
        self.sock.sendall((req + "\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WSError("connection closed during the handshake")
            head += chunk
        status = head.split(b"\r\n", 1)[0]
        if b" 101 " not in status:
            raise WSError(f"handshake refused: {status.decode(errors='replace')}")
        # RFC 6455: the server must prove it read the key (browsers refuse a wrong value)
        want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        accept = [l.split(b":", 1)[1].strip() for l in head.split(b"\r\n") if l.lower().startswith(b"sec-websocket-accept:")]
        if accept != [want]:
            raise WSError(f"wrong Sec-WebSocket-Accept {accept} (expected {want})")
        self.buf = head.split(b"\r\n\r\n", 1)[1]
        self.inbox = []
        self.next_id = 1

    def send_text(self, text, fragment_bytes=0):
        data = text.encode()
        step = fragment_bytes or max(1, len(data))
        parts = [data[i:i+step] for i in range(0, len(data), step)] or [b""]
        for index, part in enumerate(parts):
            mask = os.urandom(4)
            hdr = bytes([(0x80 if index == len(parts)-1 else 0) | (1 if index == 0 else 0)])
            n = len(part)
            if n < 126:
                hdr += bytes([0x80 | n])
            elif n < 65536:
                hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
            else:
                hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
            self.sock.sendall(hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(part)))

    def _fill(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise WSError("connection closed")
            self.buf += chunk

    def recv_frame(self):
        # A timed observation can end halfway through a frame. Keep its header in the
        # buffer until the payload is complete, so the next recv resumes the same frame.
        self._fill(2)
        b0, b1 = self.buf[:2]
        offset = 2
        n = b1 & 0x7F
        if n == 126:
            self._fill(4)
            n = struct.unpack(">H", self.buf[2:4])[0]
            offset = 4
        elif n == 127:
            self._fill(10)
            n = struct.unpack(">Q", self.buf[2:10])[0]
            offset = 10
        self._fill(offset + n)
        payload, self.buf = self.buf[offset:offset+n], self.buf[offset+n:]
        return b0 & 0x0F, payload

    def recv(self, timeout=10.0):
        """next JSON message (skips non-text frames)"""
        self.sock.settimeout(timeout)
        while True:
            op, payload = self.recv_frame()
            if op == 1:
                return json.loads(payload.decode())
            if op == 8:
                raise WSError("closed by the server")

    def wait(self, pred, timeout=10.0):
        """the first message (queued or new) matching pred; other messages are kept"""
        for i, m in enumerate(self.inbox):
            if pred(m):
                return self.inbox.pop(i)
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv(max(0.05, end - time.time()))
            if pred(m):
                return m
            if m.get("t") not in ("st", "diag"):
                self.inbox.append(m)
        raise WSError("timeout waiting for a message")

    def request(self, msg, timeout=10.0):
        """send with an id, return the ack / err"""
        msg = dict(msg)
        msg["id"] = self.next_id
        self.next_id += 1
        self.send_text(json.dumps(msg))
        return self.wait(lambda m: m.get("id") == msg["id"] and m.get("t") in ("ack", "err"), timeout)

    def hello(self):
        self.send_text(json.dumps({"t": "hello", "proto": 1, "client": "test"}))
        return self.wait(lambda m: m.get("t") == "welcome", 30)

    def close(self):
        try:
            self.sock.sendall(bytes([0x88, 0x80]) + os.urandom(4))
        except OSError:
            pass
        self.sock.close()


def http_req(host, port, method, path, body=None, headers=None, timeout=30):
    # one retry: a server at its connection limit closes new connections without answering
    for attempt in (0, 1):
        try:
            c = http.client.HTTPConnection(host, port, timeout=timeout)
            c.request(method, path, body=body, headers=headers or {})
            r = c.getresponse()
            data = r.read()
            c.close()
            return r.status, data
        except (http.client.RemoteDisconnected, ConnectionResetError, BrokenPipeError):
            if attempt:
                raise
            time.sleep(0.5)
