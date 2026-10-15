# SPDX-License-Identifier: GPL-3.0-or-later
"""The test client must retain a partial frame across a receive timeout."""
import json
import socket
import struct
import unittest

from wsclient import WS


class SplitSocket:
    def __init__(self, parts):
        self.parts = iter(parts)

    def recv(self, _size):
        part = next(self.parts)
        if isinstance(part, Exception):
            raise part
        return part

    def settimeout(self, _timeout):
        pass


class ReceiveTests(unittest.TestCase):
    def test_timeout_does_not_consume_partial_frame(self):
        for size in (20, 200, 70000):
            with self.subTest(size=size):
                message = {"t": "st", "pad": "x" * size}
                payload = json.dumps(message).encode()
                n = len(payload)
                header = (bytes([0x81, n]) if n < 126 else
                          bytes([0x81, 126]) + struct.pack(">H", n) if n < 65536 else
                          bytes([0x81, 127]) + struct.pack(">Q", n))
                following = b'\x81\x0b{"t":"ack"}'
                client = WS.__new__(WS)
                client.buf = b""
                client.sock = SplitSocket([header + payload[:3], socket.timeout(), payload[3:] + following])
                with self.assertRaises(socket.timeout):
                    client.recv()
                self.assertEqual(client.recv(), message)
                self.assertEqual(client.recv(), {"t": "ack"})


if __name__ == "__main__":
    unittest.main()
