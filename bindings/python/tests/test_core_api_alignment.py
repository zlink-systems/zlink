import asyncio
import ctypes
import importlib.util
import os
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
            "RoutedSendOp",
            "RequestOp",
            "ReplyOp",
        ):
            self.assertTrue(hasattr(zlink, name), name)
        self.assertFalse(hasattr(zlink, "RequestCallbackOp"))

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
            ctx.options.core_hwm_memory_limit_bytes = 64 * 1024 * 1024
            ctx.options.core_hwm_budget_bytes = 16 * 1024 * 1024
            self.assertEqual(
                ctx.options.core_hwm_memory_limit_bytes, 64 * 1024 * 1024
            )
            self.assertEqual(ctx.options.core_hwm_budget_bytes, 16 * 1024 * 1024)
            ctx.recalculate_auto_hwm()
            before = ctx.core_hwm_budget_snapshot()
            self.assertEqual(before.abi_version, 1)
            self.assertEqual(before.configured_core_budget_bytes, 16 * 1024 * 1024)
            self.assertEqual(len(before.reserved_u64), 8)
            ctx.reset_core_hwm_budget_metrics()
            self.assertGreater(
                ctx.core_hwm_budget_snapshot().measurement_epoch,
                before.measurement_epoch,
            )
            for invalid in (-1, 2**64, 1.5):
                with self.assertRaises((TypeError, ValueError)):
                    ctx.options.core_hwm_budget_bytes = invalid

        source = bytearray(b"payload")
        with zlink.Message.from_(source) as message:
            source[:] = b"changed"
            self.assertEqual(message.to_bytes(), b"payload")
            self.assertGreaterEqual(message.ref_count(), 1)
        self.assertEqual(message.to_bytes(), b"")

    def test_monitor_status_uses_abi_v4_byte_telemetry(self):
        with zlink.create_context() as ctx:
            with zlink.create_pair_socket(ctx) as socket:
                monitor_hwm_bytes = 12345
                with socket.monitor_open(
                    monitor_hwm_bytes=monitor_hwm_bytes
                ) as monitor:
                    status = monitor.status()
                    self.assertEqual(status.abi_version, 4)
                    self.assertEqual(status.struct_size, ctypes.sizeof(ZlinkMonitorStatus))
                    self.assertIsInstance(status.snd_pending_bytes, int)
                    self.assertIsInstance(status.rcv_pending_bytes, int)
                    self.assertEqual(
                        ctx.core_hwm_budget_snapshot().monitor_queue_applied_hwm_bytes,
                        monitor_hwm_bytes * 2,
                    )

                    # ABI 4 adds paired DEALER/ROUTER receive-flow telemetry
                    # (core-byte-hwm-flow-control-plan.ko.md §6). A PAIR
                    # socket has no completion lane to observe, so
                    # ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE (bit 5) stays
                    # unset and these fields stay at their zeroed default.
                    for field in (
                        "flow_paused_connections",
                        "flow_pause_applied_total",
                        "flow_resume_applied_total",
                        "flow_state_stale_total",
                        "flow_pause_duration_ms",
                    ):
                        value = getattr(status, field)
                        self.assertIsInstance(value, int, field)
                        self.assertEqual(value, 0, field)
                    self.assertEqual(status.detail_flags & (1 << 5), 0)

                for invalid in (-1, 2**64, 1.5):
                    with self.assertRaises((TypeError, OverflowError)):
                        socket.monitor_open(monitor_hwm_bytes=invalid)

    def test_socket_hwm_uses_unsigned_64_bit_byte_options(self):
        boundary = 2**63 + 17
        with zlink.create_context() as ctx:
            with zlink.create_pair_socket(ctx) as socket:
                self.assertGreater(socket.options.send_high_water_mark, 0)
                self.assertGreater(socket.options.receive_high_water_mark, 0)

                socket.options.send_high_water_mark = boundary
                socket.options.receive_high_water_mark = 2**64 - 1
                self.assertEqual(socket.options.send_high_water_mark, boundary)
                self.assertEqual(socket.options.receive_high_water_mark, 2**64 - 1)

                socket.options.send_high_water_mark = 0
                self.assertEqual(socket.options.send_high_water_mark, 0)

                for invalid in (-1, 2**64):
                    with self.assertRaises(OverflowError):
                        socket.options.send_high_water_mark = invalid
                with self.assertRaises(TypeError):
                    socket.options.receive_high_water_mark = 1.0

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

            with zlink.create_dealer_socket(ctx) as dealer:
                with zlink.create_router_socket(ctx) as router:
                    router.bind("inproc://python-core-11-message-request")
                    dealer.connect("inproc://python-core-11-message-request")

                    async def exchange():
                        with zlink.Message.from_(b"request-payload") as request:
                            pending = asyncio.create_task(
                                dealer.request().message(request).submit()
                            )
                            await asyncio.sleep(0)
                        request_received = zlink.create_received()
                        self.assertTrue(router.recv_into(request_received))
                        with request_received:
                            self.assertEqual(
                                request_received.to_bytes_list(),
                                [b"request-payload"],
                            )
                            with zlink.Message.from_(b"reply-payload") as reply:
                                request_received.reply().message(reply).submit()
                        parts = await pending
                        try:
                            self.assertEqual(
                                [part.to_bytes() for part in parts],
                                [b"reply-payload"],
                            )
                        finally:
                            for part in parts:
                                part.close()

                    asyncio.run(exchange())

    def test_pending_request_close_delivers_termination_without_close_error(self):
        with zlink.create_context() as ctx:
            with zlink.create_dealer_socket(ctx) as dealer:
                with zlink.create_router_socket(ctx) as router:
                    router.bind("inproc://python-core-11-pending-close")
                    dealer.connect("inproc://python-core-11-pending-close")

                    async def close_pending():
                        pending = asyncio.create_task(
                            dealer.request().message(b"pending").submit()
                        )
                        await asyncio.sleep(0)
                        received = zlink.create_received()
                        self.assertTrue(router.recv_into(received))
                        received.close()
                        dealer.close()
                        with self.assertRaises(zlink.RequestError) as raised:
                            await pending
                        self.assertEqual(
                            raised.exception.result,
                            zlink.RequestResult.TERMINATED,
                        )

                    asyncio.run(close_pending())

    def test_routed_request_has_only_the_canonical_coroutine_terminal(self):
        with zlink.create_context() as ctx:
            with zlink.create_dealer_socket(ctx) as dealer:
                request = dealer.request().message(b"payload")
                self.assertFalse(hasattr(request, "flags"))
                self.assertFalse(hasattr(request, "submit_async"))
                with self.assertRaises(TypeError):
                    request.submit(None)
                coroutine = request.submit()
                coroutine.close()

    def test_dealer_router_request_reply_uses_raw_routing_metadata(self):
        with zlink.create_context() as ctx:
            with zlink.create_dealer_socket(ctx) as dealer:
                with zlink.create_router_socket(ctx) as router:
                    router.bind("inproc://python-core-11-request")
                    dealer.connect("inproc://python-core-11-request")

                    async def exchange():
                        pending = asyncio.create_task(
                            dealer.request().message(b"ping").submit()
                        )
                        await asyncio.sleep(0)
                        received = zlink.create_received()
                        self.assertTrue(router.recv_into(received))
                        with received:
                            self.assertIsNotNone(received.routing_id)
                            self.assertIsInstance(received.request_seq, int)
                            self.assertEqual(received.to_bytes_list(), [b"ping"])
                            received.reply().message(b"pong").submit()
                        parts = await pending
                        try:
                            self.assertEqual(
                                [part.to_bytes() for part in parts], [b"pong"]
                            )
                        finally:
                            for part in parts:
                                part.close()

                    asyncio.run(exchange())

    def test_router_recv_into_keeps_storage_and_snapshot_contract(self):
        with zlink.create_context() as ctx:
            with zlink.create_dealer_socket(ctx) as dealer:
                with zlink.create_router_socket(ctx) as router:
                    router.bind("inproc://python-router-recv-owner-contract")
                    dealer.connect("inproc://python-router-recv-owner-contract")
                    received = zlink.create_received()

                    self.assertFalse(
                        router.recv_into(
                            received, flags=zlink.RecvFlags.DONT_WAIT
                        )
                    )
                    self.assertEqual(len(received), 0)

                    self.assertIsNone(
                        asyncio.run(
                            dealer.send()
                            .messages(b"first", b"second")
                            .submit()
                        )
                    )
                    self.assertTrue(router.recv_into(received))
                    self.assertIsNotNone(received.routing_id)
                    self.assertIsNone(received.request_seq)
                    self.assertEqual(
                        received.to_bytes_list(), [b"first", b"second"]
                    )
                    snapshot = received.first_part().data

                    self.assertFalse(
                        router.recv_into(
                            received, flags=zlink.RecvFlags.DONT_WAIT
                        )
                    )
                    self.assertEqual(
                        received.to_bytes_list(), [b"first", b"second"]
                    )

                    self.assertIsNone(
                        asyncio.run(
                            dealer.send().message(b"replacement").submit()
                        )
                    )
                    self.assertTrue(router.recv_into(received))
                    self.assertEqual(snapshot.tobytes(), b"first")
                    received.close()
                    received.close()

    def test_monitor_and_poller_layouts_match_core_11(self):
        self.assertEqual((ctypes.sizeof(ZlinkMsg), ctypes.alignment(ZlinkMsg)), (64, 8))
        self.assertEqual(
            (ctypes.sizeof(ZlinkRoutingId), ctypes.alignment(ZlinkRoutingId)), (256, 1)
        )
        self.assertEqual(
            (ctypes.sizeof(ZlinkMonitorEvent), ctypes.alignment(ZlinkMonitorEvent)),
            (816, 8),
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
            (16, 8),
        )
        expected_poll_item = (24, 8) if os.name == "nt" else (16, 8)
        self.assertEqual(
            (ctypes.sizeof(ZlinkPollItem), ctypes.alignment(ZlinkPollItem)),
            expected_poll_item,
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
