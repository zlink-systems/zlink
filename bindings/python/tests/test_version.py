import asyncio
import pathlib
import re
import unittest

import zlink


class VersionTests(unittest.TestCase):
    def test_version_matches_core(self):
        major, minor, patch = zlink.version()

        header = (
            pathlib.Path(__file__).resolve().parents[3] / "core" / "include" / "zlink.h"
        ).read_text(encoding="utf-8")
        expected_major = int(
            re.search(r"^#define ZLINK_VERSION_MAJOR (\d+)$", header, re.MULTILINE).group(1)
        )
        expected_minor = int(
            re.search(r"^#define ZLINK_VERSION_MINOR (\d+)$", header, re.MULTILINE).group(1)
        )
        expected_patch = int(
            re.search(r"^#define ZLINK_VERSION_PATCH (\d+)$", header, re.MULTILINE).group(1)
        )

        self.assertEqual((major, minor, patch), (expected_major, expected_minor, expected_patch))

    def test_pair_send_recv(self):
        ctx = zlink.create_context()
        with ctx:
            with zlink.create_pair_socket(ctx) as s1:
                with zlink.create_pair_socket(ctx) as s2:
                    endpoint = "inproc://py-pair"
                    s1.bind(endpoint)
                    s2.connect(endpoint)
                    payload = b"ping"
                    # PAIR `submit()` is managed: immediate admission has no
                    # completion, while backpressure waits for WRITABLE and
                    # retries the same packet.
                    asyncio.run(s1.send().message(payload).submit())
                    received = zlink.create_received()
                    self.assertTrue(s2.recv_into(received))
                    with received:
                        self.assertEqual(received.to_bytes_list(), [payload])

    def test_received_part_data_snapshot_survives_owner_close(self):
        ctx = zlink.create_context()
        with ctx:
            with zlink.create_pair_socket(ctx) as s1:
                with zlink.create_pair_socket(ctx) as s2:
                    endpoint = "inproc://py-pair-data"
                    s1.bind(endpoint)
                    s2.connect(endpoint)
                    payload = b"header-and-body-payload"
                    asyncio.run(s1.send().message(payload).submit())
                    received = zlink.create_received()
                    self.assertTrue(s2.recv_into(received))
                    with received:
                        part = received.first_part()
                        view = part.data
                        self.assertIsInstance(view, memoryview)
                        self.assertEqual(len(view), len(payload))
                        self.assertEqual(bytes(view), payload)
                        self.assertEqual(bytes(view[:6]), payload[:6])
                    self.assertEqual(bytes(view), payload)
                    with self.assertRaises(RuntimeError):
                        part.to_bytes()

if __name__ == "__main__":
    unittest.main()
