"""Focused parity tests for the receive-flow-state binding surface.

Plan: doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md §5.1, §7.3, §8.1.1.
The C ABI mirror lives in bindings/c/include; Core owns
zlink_socket_set_receive_flow_state() and zlink_receive_flow_state_t.
"""

import threading
import unittest

import zlink


def _connected_pair(ctx, endpoint):
    router = zlink.create_router_socket(ctx)
    dealer = zlink.create_dealer_socket(ctx)
    router.bind(endpoint)
    dealer.connect(endpoint)
    return dealer, router


class ReceiveFlowStateEnumParityTests(unittest.TestCase):
    def test_values_match_c_abi(self):
        self.assertEqual(int(zlink.ReceiveFlowState.RUNNING), 0)
        self.assertEqual(int(zlink.ReceiveFlowState.PAUSED), 1)


class ReceiveFlowStateDealerRouterTests(unittest.TestCase):
    def test_set_succeeds_and_repeat_is_idempotent(self):
        with zlink.create_context() as ctx:
            dealer, router = _connected_pair(
                ctx, "inproc://flow-state-dealer-router-idempotent"
            )
            with dealer, router:
                for socket in (dealer, router):
                    socket.set_receive_flow_state(zlink.ReceiveFlowState.PAUSED)
                    # Repeating the current state succeeds (plan §5.1).
                    socket.set_receive_flow_state(zlink.ReceiveFlowState.PAUSED)
                    socket.set_receive_flow_state(zlink.ReceiveFlowState.RUNNING)
                    socket.set_receive_flow_state(zlink.ReceiveFlowState.RUNNING)

    def test_dealer_router_traffic_is_unaffected_by_running_state(self):
        # HWM smoke: ordinary send/recv keeps working once RUNNING is set
        # explicitly (a no-op transition from the default state).
        with zlink.create_context() as ctx:
            dealer, router = _connected_pair(
                ctx, "inproc://flow-state-dealer-router-traffic"
            )
            with dealer, router:
                dealer.set_receive_flow_state(zlink.ReceiveFlowState.RUNNING)
                router.set_receive_flow_state(zlink.ReceiveFlowState.RUNNING)

                self.assertGreater(dealer.options.send_high_water_mark, 0)
                self.assertGreater(router.options.receive_high_water_mark, 0)

                import asyncio

                async def exchange():
                    pending = asyncio.create_task(
                        dealer.request().message(b"ping").submit()
                    )
                    await asyncio.sleep(0)
                    received = zlink.create_received()
                    self.assertTrue(router.recv_into(received))
                    with received:
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


class ReceiveFlowStateUnsupportedSocketTests(unittest.TestCase):
    def _assert_not_supported_and_traffic_unchanged(self, ctx, make_pair, endpoint):
        left, right = make_pair(ctx, endpoint)
        with left, right:
            with self.assertRaises(zlink.ConfigError) as raised:
                left.set_receive_flow_state(zlink.ReceiveFlowState.PAUSED)
            self.assertEqual(raised.exception.result, zlink.ConfigResult.NOT_SUPPORTED)

    def test_pair_socket_is_not_supported(self):
        with zlink.create_context() as ctx:
            with zlink.create_pair_socket(ctx) as left:
                with zlink.create_pair_socket(ctx) as right:
                    left.bind("inproc://flow-state-pair-unsupported")
                    right.connect("inproc://flow-state-pair-unsupported")
                    with self.assertRaises(zlink.ConfigError) as raised:
                        left.set_receive_flow_state(zlink.ReceiveFlowState.PAUSED)
                    self.assertEqual(
                        raised.exception.result, zlink.ConfigResult.NOT_SUPPORTED
                    )
                    # Existing send/recv behavior is unchanged by the rejected call.
                    self.assertTrue(left.send().message(b"still-works").submit())
                    received = zlink.create_received()
                    self.assertTrue(right.recv_into(received))
                    with received:
                        self.assertEqual(received.to_bytes_list(), [b"still-works"])

    def test_pub_sub_family_is_not_supported(self):
        with zlink.create_context() as ctx:
            for factory in (
                zlink.create_pub_socket,
                zlink.create_sub_socket,
                zlink.create_xpub_socket,
                zlink.create_xsub_socket,
            ):
                with factory(ctx) as socket:
                    with self.assertRaises(zlink.ConfigError) as raised:
                        socket.set_receive_flow_state(zlink.ReceiveFlowState.PAUSED)
                    self.assertEqual(
                        raised.exception.result, zlink.ConfigResult.NOT_SUPPORTED
                    )

    def test_stream_socket_is_not_supported(self):
        with zlink.create_context() as ctx:
            with zlink.create_stream_socket(ctx) as socket:
                with self.assertRaises(zlink.ConfigError) as raised:
                    socket.set_receive_flow_state(zlink.ReceiveFlowState.PAUSED)
                self.assertEqual(
                    raised.exception.result, zlink.ConfigResult.NOT_SUPPORTED
                )


class ReceiveFlowStateErrorMappingTests(unittest.TestCase):
    def test_invalid_argument_outside_enum_range(self):
        with zlink.create_context() as ctx:
            with zlink.create_dealer_socket(ctx) as socket:
                with self.assertRaises(zlink.ConfigError) as raised:
                    socket.set_receive_flow_state(2)
                self.assertEqual(
                    raised.exception.result, zlink.ConfigResult.INVALID_ARGUMENT
                )
                with self.assertRaises(zlink.ConfigError) as raised:
                    socket.set_receive_flow_state(-1)
                self.assertEqual(
                    raised.exception.result, zlink.ConfigResult.INVALID_ARGUMENT
                )

    def test_invalid_handle_after_close(self):
        with zlink.create_context() as ctx:
            socket = zlink.create_dealer_socket(ctx)
            socket.close()
            with self.assertRaises(zlink.ConfigError) as raised:
                socket.set_receive_flow_state(zlink.ReceiveFlowState.PAUSED)
            self.assertIn(
                raised.exception.result,
                (zlink.ConfigResult.INVALID_HANDLE, zlink.ConfigResult.INVALID_STATE),
            )

    def test_close_race_observes_one_outcome_only(self):
        # Plan §8.1: a config call racing close observes exactly one of a
        # successful local-state store or a close-related error; neither
        # partially applies nor corrupts the process.
        with zlink.create_context() as ctx:
            for _ in range(50):
                socket = zlink.create_dealer_socket(ctx)
                outcomes = []

                def setter():
                    try:
                        socket.set_receive_flow_state(zlink.ReceiveFlowState.PAUSED)
                        outcomes.append("ok")
                    except zlink.ConfigError as exc:
                        outcomes.append(exc.result)

                closer_error = []

                def closer():
                    try:
                        socket.close()
                    except zlink.CloseError as exc:
                        closer_error.append(exc)

                t1 = threading.Thread(target=setter)
                t2 = threading.Thread(target=closer)
                t1.start()
                t2.start()
                t1.join()
                t2.join()

                self.assertEqual(len(outcomes), 1)
                self.assertIn(
                    outcomes[0],
                    (
                        "ok",
                        zlink.ConfigResult.INVALID_HANDLE,
                        zlink.ConfigResult.INVALID_STATE,
                    ),
                )


class ReceiveFlowStatePublicSurfaceTests(unittest.TestCase):
    def test_no_flow_frame_api_is_public(self):
        for name in dir(zlink):
            lowered = name.lower()
            self.assertNotIn("flowframe", lowered)
            self.assertNotIn("flow_frame", lowered)

        for socket_cls in (
            zlink.PairSocket,
            zlink.DealerSocket,
            zlink.RouterSocket,
            zlink.StreamSocket,
            zlink.PubSocket,
            zlink.SubSocket,
        ):
            for name in dir(socket_cls):
                lowered = name.lower()
                self.assertNotIn("flowframe", lowered)
                self.assertNotIn("flow_frame", lowered)
                self.assertNotIn("bypass_pause", lowered)

    def test_receive_flow_state_surface_is_exactly_the_candidate_surface(self):
        self.assertTrue(hasattr(zlink, "ReceiveFlowState"))
        self.assertTrue(hasattr(zlink.DealerSocket, "set_receive_flow_state"))
        self.assertTrue(hasattr(zlink.RouterSocket, "set_receive_flow_state"))
        # No dedicated getter/receive surface was added; state is observed
        # only through existing monitor/snapshot surfaces (plan §5.1).
        self.assertFalse(hasattr(zlink.DealerSocket, "get_receive_flow_state"))
        self.assertFalse(hasattr(zlink.DealerSocket, "receive_flow_state"))


if __name__ == "__main__":
    unittest.main()
