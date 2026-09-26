# SPDX-License-Identifier: MPL-2.0

"""Projection of the Core ROUTER selected-route observation (Core ROUTER
§10.1): RouterSocket.routes_snapshot(), PollEventFlag.POLLROUTE and the route
generation carried on a received ROUTER record. Scenario mirrors
bindings/cpp/tests/contract/test_cpp_contract_router_selected_route.cpp."""

import socket as _pysocket
import time
import uuid

import zlink

WAIT_MS = 5_000


def _tcp_endpoint():
    with _pysocket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        return f"tcp://127.0.0.1:{reserve.getsockname()[1]}"


def _make_router(ctx, name):
    router = zlink.create_router_socket(ctx)
    router.set_routing_id(zlink.RoutingId.from_(name))
    router.options.linger_ms = 0
    router.options.reconnect_interval_ms = 0
    router.options.receive_timeout_ms = WAIT_MS
    router.router_options.handover = True
    return router


def _connect_as(source, peer, endpoint):
    source.router_options.connect_routing_id = zlink.RoutingId.from_(peer)
    source.connect(endpoint)


def _find_generation(routes, peer):
    expected = zlink.RoutingId.from_(peer)
    matches = [route for route in routes if route.routing_id == expected]
    for route in routes:
        assert route.route_generation != 0
    assert len(matches) <= 1
    return matches[0].route_generation if matches else 0


def _wait_route(socket, poller, events, peer, old=0, *, deadline_ms=WAIT_MS):
    deadline = time.monotonic() + deadline_ms / 1000.0
    owned_poller = poller
    owned_events = events
    if owned_poller is None:
        owned_poller = zlink.create_poller()
        owned_events = zlink.create_poll_events(1)
        owned_poller.add_socket(socket, zlink.PollEventFlag.POLLROUTE, 1)
    try:
        while True:
            generation = _find_generation(socket.routes_snapshot(), peer)
            if generation != 0 and generation != old:
                return generation
            assert time.monotonic() < deadline, f"selected route of {peer!r} did not change from {old}"
            owned_poller.wait(owned_events, 10)
    finally:
        if owned_poller is not poller:
            owned_poller.close()


def _route_ready_now(poller, events):
    count = poller.wait(events, 0)
    return count == 1 and events.revents(0) & zlink.PollEventFlag.POLLROUTE


def _send_text(socket, peer, text):
    socket.send(zlink.RoutingId.from_(peer)).message(text.encode()).submit_sync()


def _recv_text(socket, expected):
    with zlink.create_poller() as poller:
        events = zlink.create_poll_events(1)
        poller.add_socket(socket, zlink.PollEventFlag.POLLIN, 1)
        assert poller.wait(events, WAIT_MS) == 1
        assert events.revents(0) & zlink.PollEventFlag.POLLIN
    received = zlink.create_received()
    assert socket.recv_into(received, flags=zlink.RecvFlags.DONT_WAIT)
    assert received.to_bytes_list() == [expected.encode()]
    return received


def test_router_selected_route_snapshot_pollroute_and_record_generation():
    with zlink.create_context() as ctx:
        server = _make_router(ctx, "route-server")
        client = _make_router(ctx, "route-client")
        try:
            endpoint = _tcp_endpoint()
            server.bind(endpoint)

            assert client.routes_snapshot() == []

            with zlink.create_poller() as poller:
                events = zlink.create_poll_events(1)
                poller.add_socket(client, zlink.PollEventFlag.POLLROUTE, 7)
                _connect_as(client, "route-server", endpoint)

                assert poller.wait(events, WAIT_MS) == 1
                assert events.slot(0) == 7
                assert events.revents(0) & zlink.PollEventFlag.POLLROUTE

                routes = client.routes_snapshot()
                assert len(routes) == 1
                selected = _find_generation(routes, "route-server")
                assert selected != 0
                # A successful snapshot that saw every change clears readiness.
                assert not _route_ready_now(poller, events)

                _wait_route(server, None, None, "route-client")
                _send_text(server, "route-client", "selected")
                received = _recv_text(client, "selected")
                assert received.route_generation == selected
                received.close()

                # A DEALER record carries no ROUTER route generation.
                with zlink.create_dealer_socket(ctx) as dealer:
                    dealer.set_routing_id(zlink.RoutingId.from_("route-dealer"))
                    dealer.options.linger_ms = 0
                    dealer.options.receive_timeout_ms = WAIT_MS
                    dealer.connect(endpoint)
                    _wait_route(server, None, None, "route-dealer")
                    _send_text(server, "route-dealer", "dealer")
                    with zlink.create_poller() as dealer_poller:
                        dealer_events = zlink.create_poll_events(1)
                        dealer_poller.add_socket(
                            dealer, zlink.PollEventFlag.POLLIN, 2
                        )
                        assert dealer_poller.wait(dealer_events, WAIT_MS) == 1
                        assert dealer_events.revents(0) & zlink.PollEventFlag.POLLIN
                    dealer_received = zlink.create_received()
                    assert dealer.recv_into(
                        dealer_received, flags=zlink.RecvFlags.DONT_WAIT
                    )
                    assert dealer_received.route_generation == 0
                    dealer_received.close()
        finally:
            client.close()
            server.close()


def test_router_selected_route_replaced_route_changes_generation():
    with zlink.create_context() as ctx:
        old_server = _make_router(ctx, "route-server")
        new_server = _make_router(ctx, "route-server")
        client = _make_router(ctx, "route-client")
        try:
            old_endpoint = _tcp_endpoint()
            old_server.bind(old_endpoint)
            new_endpoint = _tcp_endpoint()
            new_server.bind(new_endpoint)

            with zlink.create_poller() as poller:
                events = zlink.create_poll_events(1)
                poller.add_socket(client, zlink.PollEventFlag.POLLROUTE, 9)
                _connect_as(client, "route-server", old_endpoint)
                first = _wait_route(client, poller, events, "route-server")

                # Handover: the new pipe for the same RID takes over the route.
                _connect_as(client, "route-server", new_endpoint)
                second = _wait_route(client, poller, events, "route-server", first)
                assert second != first

                _wait_route(new_server, None, None, "route-client")
                _send_text(new_server, "route-client", "new")
                received = _recv_text(client, "new")
                assert received.route_generation == second
                received.close()

                # The selected pipe ends: the standby is promoted with a new generation.
                new_server.close()
                promoted = _wait_route(client, poller, events, "route-server", second)
                assert promoted != second
        finally:
            client.close()
            old_server.close()


def test_router_selected_route_snapshot_grows_past_initial_capacity():
    with zlink.create_context() as ctx:
        client = _make_router(ctx, "route-client")
        servers = []
        try:
            names = [f"route-server-{uuid.uuid4().hex}" for _ in range(20)]
            for name in names:
                server = _make_router(ctx, name)
                servers.append(server)
                endpoint = _tcp_endpoint()
                server.bind(endpoint)
                _connect_as(client, name, endpoint)
            for name in names:
                _wait_route(client, None, None, name)
            routes = client.routes_snapshot()
            assert len(routes) == 20
            assert {route.routing_id for route in routes} == {
                zlink.RoutingId.from_(name) for name in names
            }
        finally:
            client.close()
            for server in servers:
                server.close()
