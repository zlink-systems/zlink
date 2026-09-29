# SPDX-License-Identifier: MPL-2.0

import uuid

import pytest
import zlink


def test_disconnect_rid_not_found_uses_connect_error_after_peer_disconnect():
    endpoint = f"inproc://disconnect-rid-{uuid.uuid4().hex}"
    with zlink.create_context() as ctx:
        with zlink.create_router_socket(ctx) as router, zlink.create_dealer_socket(
            ctx
        ) as dealer:
            rid = zlink.RoutingId.from_(f"disconnect-rid-{uuid.uuid4().hex}")
            dealer.set_routing_id(rid)
            router.bind(endpoint)
            dealer.connect(endpoint)

            with dealer.monitor_open(
                zlink.MonitorEventMask.DISCONNECTED
            ) as monitor, zlink.create_poller() as poller:
                poller.add_socket(monitor, zlink.PollEventFlag.POLLIN, 1)
                dealer.send().message(b"ready").submit_sync()
                received = zlink.create_received()
                try:
                    assert router.recv_into(received)
                    assert received.routing_id == rid
                    router.disconnect_rid(rid)

                    events = zlink.create_poll_events(1)
                    assert poller.wait(events, 5000) == 1
                    event = monitor.recv(flags=zlink.RecvFlags.DONT_WAIT)
                    assert event is not None
                    assert event.event == zlink.MonitorEventMask.DISCONNECTED

                    with pytest.raises(zlink.ConnectError) as error:
                        router.disconnect_rid(rid)
                    assert error.value.result == zlink.ConnectResult.NOT_FOUND
                    assert int(error.value.result) == 605
                finally:
                    received.close()
