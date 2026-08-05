import unittest

import zlink
from zlink._runtime.sockets.socket_base import _native_socket_type


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
