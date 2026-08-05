# SPDX-License-Identifier: MPL-2.0

"""Regression tests for blocking sends inside receive callbacks."""

import errno
import socket
import struct
import threading
import time
import unittest

import zlink


def _inproc_endpoint(name):
    return f"inproc://callback-send-{name}"


def _tcp_endpoint():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port, f"tcp://127.0.0.1:{port}"


class CallbackSendTests(unittest.TestCase):
    def setUp(self):
        self.ctx = zlink.create_context()

    def tearDown(self):
        if hasattr(self, "ctx") and self.ctx is not None:
            self.ctx.close()

    def test_stream_send_inside_on_packet_raises_explicit_error(self):
        port, endpoint = _tcp_endpoint()
        stream = zlink.create_stream_socket(self.ctx)

        done = threading.Event()
        callback_error = []

        def on_message(routing_id, header, body):
            try:
                self.assertIsNotNone(routing_id)
                self.assertEqual(header.to_bytes(), b"")
                self.assertEqual(body.to_bytes(), b"ping")
                stream.send(routing_id).message(b"pong").submit()
            except Exception as exc:
                callback_error.append(exc)
            finally:
                done.set()

        stream.bind(endpoint)
        stream.on_packet(on_message)
        time.sleep(0.05)

        with socket.create_connection(("127.0.0.1", port), timeout=3.0) as client:
            client.sendall(struct.pack("!HI", 0, len(b"ping")) + b"ping")
            self.assertTrue(done.wait(3.0), "callback timed out")

        self.assertEqual(len(callback_error), 1, f"callback raised: {callback_error}")
        self.assertIsInstance(callback_error[0], zlink.ZlinkError)
        self.assertEqual(callback_error[0].native_errno, errno.EDEADLK)

        stream.close()

if __name__ == "__main__":
    unittest.main()
