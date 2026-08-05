import ctypes
import importlib.util
import queue
import time
import unittest

import zlink
from zlink._native.ffi import (
    ZlinkMonitorEvent,
    ZlinkMonitorStatus,
    ZlinkMsg,
    ZlinkPollItem,
    ZlinkPollerEvent,
    ZlinkRoutingId,
    ZlinkSocketMonitorOpenOptions,
    lib,
)


def _wait_for(value, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if value():
            return
        time.sleep(0.001)
    raise AssertionError("timed out waiting for callback")


class CoreApiAlignmentTests(unittest.TestCase):
    def test_public_surface_is_core_raw_only(self):
        service_names = {
            name
            for name in dir(zlink)
            if any(token in name.lower() for token in ("spot", "actor", "discovery"))
        }
        self.assertEqual(service_names, set())
        self.assertIsNone(importlib.util.find_spec("zlink.contracts.service"))
        self.assertIsNone(importlib.util.find_spec("zlink._runtime.service"))

        for name in (
            "PairSocket",
            "DealerSocket",
            "RouterSocket",
            "StreamSocket",
            "PubSocket",
            "SubSocket",
            "MonitorSocket",
            "Message",
            "Received",
            "RoutingId",
            "SendOp",
            "RequestOp",
            "RequestCallbackOp",
            "ReplyOp",
        ):
            self.assertTrue(hasattr(zlink, name), name)

        self.assertFalse(hasattr(zlink.Message, "get_property"))
        self.assertTrue(hasattr(zlink.Message, "ref_count"))
        self.assertFalse(hasattr(zlink.PairSocket, "try_send"))
        self.assertFalse(hasattr(zlink.PairSocket, "try_recv"))
        self.assertFalse(hasattr(zlink.RouterSocket, "send_to_spot"))
        self.assertFalse(hasattr(zlink.StreamSocket, "bind_actor"))

    def test_routing_id_validation_and_display(self):
        rid = zlink.RoutingId.from_(b"abc")
        self.assertEqual(rid.size, 3)
        self.assertEqual(str(rid), "abc")
        self.assertEqual(zlink.RoutingId.from_hex("004142"), zlink.RoutingId.from_(b"\0AB"))
        self.assertEqual(zlink.RoutingId.from_hex("a" * 510).size, 255)
        for invalid in (b"", b"a" * 256):
            with self.assertRaises(ValueError):
                zlink.RoutingId.from_(invalid)

    def test_context_and_message_contract(self):
        with zlink.create_context() as ctx:
            self.assertGreater(ctx.options.max_sockets, 0)
            ctx.options.auto_hwm_msg_unit_bytes = 64
            self.assertEqual(ctx.options.auto_hwm_msg_unit_bytes, 64)

        source = bytearray(b"payload")
        with zlink.Message.from_(source) as message:
            source[:] = b"changed"
            self.assertEqual(message.to_bytes(), b"payload")
            self.assertGreaterEqual(message.ref_count(), 1)
        self.assertEqual(message.to_bytes(), b"")

    def test_pair_multipart_roundtrip_uses_caller_received_storage(self):
        with zlink.create_context() as ctx:
            with zlink.create_pair_socket(ctx) as left:
                with zlink.create_pair_socket(ctx) as right:
                    left.bind("inproc://python-core-11-pair")
                    right.connect("inproc://python-core-11-pair")
                    self.assertTrue(
                        left.send().messages(b"first", b"second").submit()
                    )
                    received = zlink.create_received()
                    self.assertTrue(right.recv_into(received))
                    with received:
                        self.assertEqual(received.to_bytes_list(), [b"first", b"second"])
                    self.assertEqual(len(received), 0)

    def test_message_builder_and_request_reply_accept_native_messages(self):
        with zlink.create_context() as ctx:
            with zlink.create_pair_socket(ctx) as left:
                with zlink.create_pair_socket(ctx) as right:
                    left.bind("inproc://python-core-11-message-builder")
                    right.connect("inproc://python-core-11-message-builder")
                    with zlink.Message.from_(b"builder-payload") as message:
                        self.assertTrue(left.send().message(message).submit())
                    received = zlink.create_received()
                    self.assertTrue(right.recv_into(received))
                    with received:
                        self.assertEqual(received.to_bytes_list(), [b"builder-payload"])

            callbacks = queue.Queue()
            with zlink.create_dealer_socket(ctx) as dealer:
                with zlink.create_router_socket(ctx) as router:
                    router.bind("inproc://python-core-11-message-request")
                    dealer.connect("inproc://python-core-11-message-request")

                    def on_reply(result, parts):
                        try:
                            callbacks.put((result, [part.to_bytes() for part in parts]))
                        finally:
                            for part in parts:
                                part.close()

                    with zlink.Message.from_(b"request-payload") as request:
                        self.assertTrue(
                            dealer.request().message(request).submit(on_reply)
                        )
                    request_received = zlink.create_received()
                    self.assertTrue(router.recv_into(request_received))
                    with request_received:
                        self.assertEqual(
                            request_received.to_bytes_list(), [b"request-payload"]
                        )
                        with zlink.Message.from_(b"reply-payload") as reply:
                            request_received.reply().message(reply).submit()

                    _wait_for(lambda: not callbacks.empty())
                    result, parts = callbacks.get_nowait()
                    self.assertEqual(result, zlink.RequestResult.OK)
                    self.assertEqual(parts, [b"reply-payload"])

    def test_pending_request_close_delivers_termination_without_close_error(self):
        callbacks = queue.Queue()
        with zlink.create_context() as ctx:
            with zlink.create_dealer_socket(ctx) as dealer:
                with zlink.create_router_socket(ctx) as router:
                    router.bind("inproc://python-core-11-pending-close")
                    dealer.connect("inproc://python-core-11-pending-close")
                    dealer.request().message(b"pending").submit(
                        lambda result, parts: callbacks.put((result, parts))
                    )
                    received = zlink.create_received()
                    self.assertTrue(router.recv_into(received))
                    received.close()

                    dealer.close()

                    _wait_for(lambda: not callbacks.empty())
                    result, parts = callbacks.get_nowait()
                    self.assertEqual(result, zlink.RequestResult.TERMINATED)
                    self.assertEqual(parts, [])

    def test_request_rejects_missing_callback_before_native_submission(self):
        with zlink.create_context() as ctx:
            with zlink.create_dealer_socket(ctx) as dealer:
                with self.assertRaises(TypeError):
                    dealer.request().message(b"payload").submit(None)
                with self.assertRaises(TypeError):
                    dealer.request().message(b"payload").flags(0).submit(None)

    def test_dealer_router_request_reply_uses_raw_routing_metadata(self):
        callbacks = queue.Queue()
        with zlink.create_context() as ctx:
            with zlink.create_dealer_socket(ctx) as dealer:
                with zlink.create_router_socket(ctx) as router:
                    router.bind("inproc://python-core-11-request")
                    dealer.connect("inproc://python-core-11-request")
                    dealer.request().message(b"ping").submit(
                        lambda result, parts: callbacks.put(
                            (result, [part.to_bytes() for part in parts])
                        )
                    )
                    received = zlink.create_received()
                    self.assertTrue(router.recv_into(received))
                    with received:
                        self.assertIsNotNone(received.routing_id)
                        self.assertIsInstance(received.request_seq, int)
                        self.assertEqual(received.to_bytes_list(), [b"ping"])
                        received.reply().message(b"pong").submit()
                    _wait_for(lambda: not callbacks.empty())
                    result, parts = callbacks.get_nowait()
                    self.assertEqual(result, zlink.RequestResult.OK)
                    self.assertEqual(parts, [b"pong"])

    def test_monitor_and_poller_layouts_match_core_11(self):
        self.assertEqual((ctypes.sizeof(ZlinkMsg), ctypes.alignment(ZlinkMsg)), (64, 8))
        self.assertEqual(
            (ctypes.sizeof(ZlinkRoutingId), ctypes.alignment(ZlinkRoutingId)), (256, 1)
        )
        self.assertEqual(
            (ctypes.sizeof(ZlinkMonitorEvent), ctypes.alignment(ZlinkMonitorEvent)),
            (784, 8),
        )
        self.assertEqual(
            (ctypes.sizeof(ZlinkMonitorStatus), ctypes.alignment(ZlinkMonitorStatus)),
            (232, 8),
        )
        self.assertEqual(
            (
                ctypes.sizeof(ZlinkSocketMonitorOpenOptions),
                ctypes.alignment(ZlinkSocketMonitorOpenOptions),
            ),
            (4, 4),
        )
        self.assertEqual(
            (ctypes.sizeof(ZlinkPollItem), ctypes.alignment(ZlinkPollItem)), (16, 8)
        )
        self.assertEqual(
            (ctypes.sizeof(ZlinkPollerEvent), ctypes.alignment(ZlinkPollerEvent)),
            (48, 8),
        )

    def test_timer_and_poller_are_raw_core_facades(self):
        with zlink.create_timer() as timer:
            self.assertIsNone(timer.recv())
            timer.start(1_000_000, 1)
            _wait_for(lambda: timer.recv() is not None)

        with zlink.create_context() as ctx, zlink.create_pair_socket(ctx) as pair:
            with zlink.create_poller() as poller:
                events = zlink.create_poll_events(1)
                poller.add_socket(pair, zlink.PollEventFlag.POLLIN, 7)
                self.assertEqual(poller.size(), 1)
                self.assertEqual(poller.wait(events, 0), 0)
                self.assertEqual(events.ready_count, 0)
                poller.remove_socket(pair)
                self.assertEqual(poller.size(), 0)

    def test_native_raw_symbols_have_no_framework_service_symbols(self):
        native = lib()
        for name in (
            "zlink_spot_new",
            "zlink_actor_new",
            "zlink_service_monitor_open",
            "zlink_msg_gets",
        ):
            self.assertFalse(hasattr(native, name), name)


if __name__ == "__main__":
    unittest.main()
