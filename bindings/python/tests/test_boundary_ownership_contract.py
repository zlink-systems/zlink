import errno
import threading
import time
import unittest
import uuid

import zlink
from zlink._runtime.handles.native_support import _msg_to_bytes
from zlink._runtime.messaging.native_parts import _materialize_native_parts
from zlink._runtime.sockets.socket_base import _close_native_parts, _native_socket_type


class BoundaryValidationContractTests(unittest.TestCase):
    def test_routing_id_byte_boundaries_are_checked_before_native_call(self):
        self.assertEqual(zlink.RoutingId.from_(b"a" * 255).size, 255)

        for invalid in (b"", b"a" * 256):
            with self.assertRaises(ValueError):
                zlink.RoutingId.from_(invalid)

    def test_endpoint_rejects_fixed_buffer_overflow(self):
        with zlink.create_context() as ctx:
            with zlink.create_router_socket(ctx) as socket:
                with self.assertRaises(ValueError):
                    socket.bind("inproc://" + ("x" * 248))
                with self.assertRaises(ValueError):
                    socket.bind("inproc://bad\0endpoint")

    def test_socket_type_values_are_not_reinterpreted_before_native_call(self):
        self.assertEqual(_native_socket_type(zlink.SocketType.ANY), 0)
        self.assertEqual(_native_socket_type(zlink.SocketType.PAIR), 0x1001)
        self.assertEqual(_native_socket_type(1), 1)

    def test_removed_channel_name_surface_is_not_public(self):
        self.assertFalse(hasattr(zlink.PairSocket, "set_channel_name"))
        self.assertFalse(hasattr(zlink.PairSocket, "get_channel_name"))
        self.assertFalse(hasattr(zlink.RouterSocket, "set_channel_name"))
        self.assertFalse(hasattr(zlink.RouterSocket, "get_channel_name"))

    def test_typed_numeric_options_reject_native_width_overflow(self):
        with zlink.create_context() as ctx:
            with self.assertRaises(OverflowError):
                ctx.options.io_threads = 1 << 31
            with self.assertRaises(OverflowError):
                ctx.options.io_threads = -(1 << 31) - 1

            with zlink.create_router_socket(ctx) as socket:
                socket.options.max_message_size = (1 << 63) - 1
                self.assertEqual(socket.options.max_message_size, (1 << 63) - 1)

                with self.assertRaises(OverflowError):
                    socket.options.max_message_size = 1 << 63
                with self.assertRaises(OverflowError):
                    socket.options.max_message_size = -(1 << 63) - 1

    def test_submit_retry_mode_enum_values_are_public(self):
        self.assertEqual(zlink.SubmitRetryMode.OFF, 0)
        self.assertEqual(zlink.SubmitRetryMode.LOCAL_FAILURE, 1)

    def test_submit_retry_options_roundtrip_and_validate_native_bounds(self):
        with zlink.create_context() as ctx:
            with zlink.create_router_socket(ctx) as socket:
                self.assertEqual(socket.options.submit_retry_mode,
                                 zlink.SubmitRetryMode.OFF)
                self.assertEqual(socket.options.submit_retry_timeout_ms, 0)
                self.assertEqual(socket.options.submit_retry_attempts, 0)

                socket.options.submit_retry_mode = (
                    zlink.SubmitRetryMode.LOCAL_FAILURE)
                socket.options.submit_retry_timeout_ms = 250
                socket.options.submit_retry_attempts = 16

                self.assertEqual(socket.options.submit_retry_mode,
                                 zlink.SubmitRetryMode.LOCAL_FAILURE)
                self.assertEqual(socket.options.submit_retry_timeout_ms, 250)
                self.assertEqual(socket.options.submit_retry_attempts, 16)

                with self.assertRaises(zlink.ConfigError):
                    socket.options.submit_retry_mode = 2
                with self.assertRaises(zlink.ConfigError):
                    socket.options.submit_retry_timeout_ms = -1
                with self.assertRaises(zlink.ConfigError):
                    socket.options.submit_retry_attempts = 17


class OwnershipContractTests(unittest.TestCase):
    def test_received_native_parts_clone_survives_close_and_staging_retry(self):
        endpoint = f"inproc://python-received-clone-{uuid.uuid4()}"
        first = b"a" * 1024
        second = b""

        with zlink.create_context() as context:
            with zlink.create_pair_socket(context) as sender:
                with zlink.create_pair_socket(context) as receiver:
                    receiver.bind(endpoint)
                    sender.connect(endpoint)
                    time.sleep(0.05)
                    sender.send().messages(first, second).submit_sync()

                    received = zlink.create_received()
                    self.assertTrue(receiver.recv_into(received))
                    staged = _materialize_native_parts(received.parts)
                    try:
                        self.assertEqual(
                            [_msg_to_bytes(part) for part in staged],
                            [first, second],
                        )
                        # Closing one rejected/abandoned staging attempt must
                        # not consume the caller-owned received envelope.
                        _close_native_parts(staged)
                        staged = []
                        self.assertEqual(received.to_bytes_list(), [first, second])

                        source_parts = received.parts
                        staged = _materialize_native_parts(source_parts)
                        received.close()
                        self.assertEqual(
                            [_msg_to_bytes(part) for part in staged],
                            [first, second],
                        )
                        with self.assertRaisesRegex(
                            RuntimeError, "received message is closed"
                        ):
                            _materialize_native_parts(source_parts)
                    finally:
                        _close_native_parts(staged)
                        received.close()

    def test_raw_reply_forwards_received_native_multipart_without_source_copy(self):
        endpoint = f"inproc://python-received-reply-{uuid.uuid4()}"
        payloads = [
            (bytes([index]) * 1024, b"")
            for index in range(1, 33)
        ]

        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    router.bind(endpoint)
                    dealer.connect(endpoint)
                    time.sleep(0.05)
                    failures = []

                    def reply_all():
                        received = zlink.create_received()
                        try:
                            for expected in payloads:
                                self.assertTrue(router.recv_into(received))
                                received.reply().messages(*received.parts).submit()
                                self.assertEqual(received.to_bytes_list(), list(expected))
                                received.close()
                        except BaseException as exc:
                            failures.append(exc)
                        finally:
                            received.close()

                    replier = threading.Thread(target=reply_all)
                    replier.start()
                    for expected in payloads:
                        reply = (
                            dealer.request()
                            .messages(*expected)
                            .timeout(1.0)
                            .submit_sync()
                        )
                        try:
                            self.assertEqual(
                                [part.to_bytes() for part in reply], list(expected)
                            )
                        finally:
                            for part in reply:
                                part.close()
                    replier.join(timeout=5)
                    self.assertFalse(replier.is_alive())
                    if failures:
                        raise failures[0]

    def test_concurrent_multipart_binding_staging_preserves_public_parts(self):
        endpoint = f"inproc://python-concurrent-multipart-{uuid.uuid4()}"
        with zlink.create_context() as context:
            with zlink.create_pair_socket(context) as sender:
                with zlink.create_pair_socket(context) as receiver:
                    sender.options.send_high_water_mark = 1 << 20
                    receiver.options.receive_high_water_mark = 1 << 20
                    receiver.bind(endpoint)
                    sender.connect(endpoint)
                    time.sleep(0.05)

                    self.assertFalse(
                        hasattr(sender, "_outbound_record_attempt_gate")
                    )
                    lock = threading.Lock()
                    successes = set()
                    rejected = 0
                    failures = []

                    def submit_records(worker):
                        nonlocal rejected
                        local_successes = set()
                        local_rejected = 0
                        try:
                            for index in range(500):
                                prefix = f"record-{worker}-{index:03d}"
                                first = zlink.Message.from_(
                                    (prefix + "-a").encode()
                                )
                                second = zlink.Message.from_(
                                    (prefix + "-b").encode()
                                )
                                try:
                                    sender.send().messages(
                                        first, second
                                    ).submit_sync()
                                    local_successes.add(prefix)
                                except zlink.SubmitError as error:
                                    if (
                                        error.result
                                        != zlink.SubmitResult.INVALID_ARGUMENT
                                        or error.native_errno != errno.EINVAL
                                    ):
                                        raise
                                    if first.to_bytes() != (
                                        prefix + "-a"
                                    ).encode() or second.to_bytes() != (
                                        prefix + "-b"
                                    ).encode():
                                        raise AssertionError(
                                            "binding staging lost caller-owned parts after Core rejection"
                                        )
                                    local_rejected += 1
                                finally:
                                    first.close()
                                    second.close()
                        except BaseException as error:
                            with lock:
                                failures.append(error)
                            return
                        with lock:
                            successes.update(local_successes)
                            rejected += local_rejected

                    threads = [
                        threading.Thread(target=submit_records, args=(worker,))
                        for worker in range(8)
                    ]
                    for thread in threads:
                        thread.start()
                    for thread in threads:
                        thread.join()

                    if failures:
                        raise failures[0]
                    self.assertGreater(len(successes), 0)
                    self.assertGreater(rejected, 0)

                    received_prefixes = set()
                    received = zlink.create_received()
                    for _ in range(len(successes)):
                        self.assertTrue(receiver.recv_into(received))
                        parts = received.to_bytes_list()
                        self.assertEqual(len(parts), 2)
                        self.assertTrue(parts[0].endswith(b"-a"))
                        prefix = parts[0][:-2].decode()
                        self.assertEqual(parts[1], (prefix + "-b").encode())
                        received_prefixes.add(prefix)
                    received.close()
                    self.assertEqual(received_prefixes, successes)

    def test_message_copy_owns_bytes_independently_of_source_buffer(self):
        source = bytearray(b"payload")
        with zlink.Message.from_(source) as message:
            source[:] = b"changed"
            self.assertEqual(message.to_bytes(), b"payload")

        self.assertEqual(message.size(), 0)
        message.close()
        self.assertEqual(message.to_bytes(), b"")

    def test_message_try_copy_to_reports_count_or_capacity_miss(self):
        with zlink.Message.from_(b"payload") as message:
            destination = bytearray(7)
            self.assertEqual(7, message.try_copy_to(destination))
            self.assertEqual(destination, b"payload")
            self.assertIsNone(message.try_copy_to(bytearray(6)))

            with self.assertRaises(TypeError):
                message.try_copy_to(bytes(7))

    def test_caller_provided_received_storage_close_is_idempotent_when_empty(self):
        received = zlink.create_received()

        self.assertEqual(len(received), 0)
        with self.assertRaises(zlink.RecvError) as raised:
            received.single_part_or_throw()
        self.assertEqual(raised.exception.result, zlink.RecvResult.NO_DATA)

        received.close()
        received.close()


if __name__ == "__main__":
    unittest.main()
