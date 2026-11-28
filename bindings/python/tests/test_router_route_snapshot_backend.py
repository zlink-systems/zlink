# SPDX-License-Identifier: MPL-2.0

import zlink


def test_router_snapshots_and_received_records_expose_the_same_route_generation():
    with (
        zlink.create_context() as context,
        zlink.create_router_socket(context) as router,
        zlink.create_dealer_socket(context) as dealer,
        zlink.create_poller() as poller,
        zlink.create_received() as received,
    ):
        dealer.options.linger_ms = router.options.linger_ms = 0
        router.options.receive_timeout_ms = 1000
        assert router.routes_snapshot() == []
        router.bind("inproc://route-snapshot-backend")
        dealer.set_routing_id(b"dealer")
        dealer.connect("inproc://route-snapshot-backend")
        poller.add_socket(router, zlink.PollEventFlag.POLLROUTE, 1)

        dealer.send().message(b"data").submit_sync()
        assert poller.wait(zlink.create_poll_events(1), 1000) == 1
        route, = router.routes_snapshot()
        assert route.routing_id == b"dealer"
        assert route.route_generation != 0
        assert router.recv_into(received)
        assert received.route_generation == route.route_generation
