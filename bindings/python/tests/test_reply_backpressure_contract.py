# SPDX-License-Identifier: MPL-2.0

from concurrent.futures import ThreadPoolExecutor
import errno

import pytest
import zlink


@pytest.mark.parametrize("requester_role", ["dealer", "router"])
def test_raw_reply_admission_depends_on_the_requester_lane(requester_role):
    factory = getattr(zlink, f"create_{requester_role}_socket")
    responder_rid = zlink.RoutingId.from_(b"responder")
    endpoint = "inproc://reply-lane-backpressure"
    with (
        zlink.create_context() as context,
        factory(context) as requester,
        zlink.create_router_socket(context) as responder,
        zlink.create_received() as request,
        zlink.create_received() as data,
    ):
        for peer in (requester, responder):
            peer.options.linger_ms = 0
            peer.options.receive_timeout_ms = 1000
        responder.options.send_high_water_mark = 4096
        requester.options.receive_high_water_mark = 4096
        responder.set_routing_id(responder_rid)
        requester.set_routing_id(zlink.RoutingId.from_(b"requester"))
        if requester_role == "router":
            requester.router_options.connect_routing_id = responder_rid
        responder.bind(endpoint)
        requester.connect(endpoint)
        if requester_role == "router":
            # Wait for the selected route through the public route observer.
            with zlink.create_poller() as poller:
                poller.add_socket(requester, zlink.PollEventFlag.POLLROUTE, 1)
                assert poller.wait(zlink.create_poll_events(1), 1000) == 1
                assert responder_rid in [route.routing_id for route in requester.routes_snapshot()]
            operation = requester.request(responder_rid)
        else:
            operation = requester.request()

        with ThreadPoolExecutor(1) as executor:
            pending = executor.submit(operation.message(b"request").timeout(3).submit_sync)
            assert responder.recv_into(request)
            assert request.to_bytes_list() == [b"request"]

            responder.options.send_timeout_ms = 0
            payload = b"d" * 1024
            for queued in range(64):
                try:
                    responder.send(request.routing_id).message(payload).submit_sync()
                except zlink.SubmitError as error:
                    assert error.result is zlink.SubmitResult.BACKPRESSURED
                    assert queued > 0
                    break
            else:
                pytest.fail("Application pipe did not reach its configured HWM")

            if requester_role == "dealer":
                with pytest.raises(zlink.SubmitError) as error:
                    request.reply().message(b"blocked").submit()
                assert error.value.result is zlink.SubmitResult.BACKPRESSURED
                assert error.value.internal_errno == errno.EAGAIN
                for _ in range(queued):
                    assert requester.recv_into(data)
                    assert data.to_bytes_list() == [payload]
                responder.options.send_timeout_ms = 1000

            request.reply().messages(b"reply", b"").submit()
            parts = pending.result(timeout=3)
            try:
                assert [part.to_bytes() for part in parts] == [b"reply", b""]
            finally:
                for part in parts:
                    part.close()
