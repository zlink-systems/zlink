"""Public reply ownership and error regression tests, without pytest."""

import concurrent.futures
import asyncio
import threading
import unittest
import uuid

import zlink


class ReplyNativeTests(unittest.TestCase):
    def exchange(self, payloads, reply):
        with zlink.create_context() as context:
            with zlink.create_router_socket(context) as server:
                with zlink.create_dealer_socket(context) as client:
                    endpoint = "inproc://reply-native-" + uuid.uuid4().hex
                    server.bind(endpoint)
                    client.connect(endpoint)
                    with concurrent.futures.ThreadPoolExecutor(1) as executor:
                        pending = executor.submit(
                            lambda: client.request().messages(*payloads)
                            .timeout(2).submit_sync()
                        )
                        with zlink.create_received() as received:
                            self.assertTrue(server.recv_into(received))
                            reply(server, received)
                        parts = pending.result(timeout=3)
                        try:
                            self.assertEqual([part.to_bytes() for part in parts], payloads)
                        finally:
                            for part in parts:
                                part.close()

    def test_received_parts_remain_owned_until_close(self):
        for size in (0, 64, 65536):
            with self.subTest(size=size):
                expected = [b"x" * size, b""]

                def reply(server, received):
                    parts = tuple(received)
                    received.reply().messages(*parts).submit()
                    self.assertEqual([part.to_bytes() for part in parts], expected)

                self.exchange(expected, reply)

    def test_part_counts_cover_inline_and_heap_storage(self):
        for count in (1, 2, 9):
            with self.subTest(count=count):
                expected = [bytes([index]) * 64 for index in range(count)]
                self.exchange(expected, lambda server, received:
                              received.reply().messages(*received).submit())

    def test_message_and_mutable_input_remain_usable(self):
        expected = [b"mutable", b"message", b""]

        def reply(server, received):
            data = bytearray(expected[0])
            with zlink.Message.from_(expected[1]) as message:
                received.reply().messages(data, message, memoryview(b"")).submit()
                self.assertEqual(message.to_bytes(), expected[1])
                data[:] = b"changed"

        self.exchange(expected, reply)

    def test_wrong_owner_fails_before_consuming_token(self):
        def reply(server, received):
            with zlink.create_context() as context:
                with zlink.create_router_socket(context) as other:
                    with self.assertRaises(zlink.SubmitError) as raised:
                        other.reply(received.routing_id, received.reply_token)
                    self.assertEqual(raised.exception.result, zlink.SubmitResult.INVALID_ARGUMENT)
            received.reply().messages(*received).submit()

        self.exchange([b"owner", b""], reply)

    def test_builder_is_single_use_and_consumed_token_fails(self):
        def reply(server, received):
            operation = received.reply().messages(*received)
            operation.submit()
            with self.assertRaises(zlink.SubmitError) as raised:
                operation.submit()
            self.assertEqual(raised.exception.result, zlink.SubmitResult.INVALID_STATE)
            with self.assertRaises(zlink.SubmitError):
                received.reply().messages(*received).submit()
            self.assertEqual(received.to_bytes_list(), [b"once", b""])

        self.exchange([b"once", b""], reply)

    def test_routing_id_buffer_validation_preserves_reply(self):
        def reply(server, received):
            with zlink.Message.from_(b"rid") as payload:
                for invalid in (b"", bytes(256)):
                    with self.assertRaises(ValueError):
                        server.reply(invalid, received.reply_token).message(payload).submit()
                    self.assertEqual(payload.to_bytes(), b"rid")
                raw = bytes(received.routing_id)
                spaced = bytearray(len(raw) * 2)
                spaced[::2] = raw
                server.reply(memoryview(spaced)[::2], received.reply_token).message(payload).submit()
                self.assertEqual(payload.to_bytes(), b"rid")

        self.exchange([b"rid"], reply)

    def test_async_requests_and_cancelled_late_reply(self):
        async def exercise():
            with zlink.create_context() as context:
                with zlink.create_router_socket(context) as server:
                    with zlink.create_dealer_socket(context) as client:
                        endpoint = "inproc://reply-async-" + uuid.uuid4().hex
                        server.bind(endpoint)
                        client.connect(endpoint)
                        arrived = asyncio.Event()
                        release = threading.Event()
                        loop = asyncio.get_running_loop()

                        def serve():
                            with zlink.create_received() as received:
                                for index in range(65):
                                    self.assertTrue(server.recv_into(received))
                                    if index == 0:
                                        loop.call_soon_threadsafe(arrived.set)
                                        self.assertTrue(release.wait(2))
                                    received.reply().messages(*received).submit()
                                    received.close()

                        with concurrent.futures.ThreadPoolExecutor(1) as executor:
                            server_done = executor.submit(serve)
                            cancelled = asyncio.ensure_future(
                                client.request()
                                .messages(b"cancel", b"")
                                .timeout(2)
                                .submit()
                                .reply
                            )
                            try:
                                await asyncio.wait_for(arrived.wait(), 2)
                                cancelled.cancel()
                                with self.assertRaises(asyncio.CancelledError):
                                    await cancelled
                            finally:
                                release.set()

                            async def request(index):
                                expected = [str(index).encode(), b""]
                                submission = (
                                    client.request()
                                    .messages(*expected)
                                    .timeout(2)
                                    .submit()
                                )
                                parts = await submission.reply
                                try:
                                    self.assertEqual([part.to_bytes() for part in parts], expected)
                                finally:
                                    for part in parts:
                                        part.close()

                            await asyncio.wait_for(
                                asyncio.gather(*(request(index) for index in range(64))), 2
                            )
                            server_done.result(timeout=3)

        asyncio.run(exercise())


if __name__ == "__main__":
    unittest.main()
