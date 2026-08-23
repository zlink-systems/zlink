import asyncio
import gc
import uuid

import zlink


def _endpoint(label):
    return f"inproc://python-{label}-{uuid.uuid4()}"


def _lease_count(context):
    return context.core_hwm_budget_snapshot().outstanding_application_lease_count


def test_retained_receive_is_explicit_and_keeps_raw_lease_private():
    for socket_type in (
        zlink.PairSocket,
        zlink.DealerSocket,
        zlink.RouterSocket,
        zlink.StreamSocket,
    ):
        assert hasattr(socket_type, "recv_retained_into")
    for socket_type in (zlink.SubSocket, zlink.XSubSocket):
        assert hasattr(socket_type, "subscribe_retained_into")

    received = zlink.create_received()
    topic_message = zlink.create_topic_message()
    assert not any("lease" in name.lower() for name in dir(received))
    assert not any("lease" in name.lower() for name in dir(topic_message))


def test_ordinary_receive_returns_credit_immediately_and_retained_releases_once():
    with zlink.create_context() as context:
        with zlink.create_pair_socket(context) as sender:
            with zlink.create_pair_socket(context) as receiver:
                endpoint = _endpoint("retained-lifetime")
                sender.bind(endpoint)
                receiver.connect(endpoint)
                baseline = _lease_count(context)

                asyncio.run(sender.send().messages(b"ordinary-1", b"ordinary-2").submit())
                ordinary = zlink.create_received()
                assert receiver.recv_into(ordinary)
                assert ordinary.to_bytes_list() == [b"ordinary-1", b"ordinary-2"]
                assert _lease_count(context) == baseline
                ordinary.close()

                asyncio.run(sender.send().messages(b"retained-1", b"retained-2").submit())
                retained = zlink.create_received()
                assert receiver.recv_retained_into(retained)
                assert retained.to_bytes_list() == [b"retained-1", b"retained-2"]
                assert _lease_count(context) == baseline + 2

                retained.first_part().close()
                assert _lease_count(context) == baseline + 2
                retained.close()
                retained.close()
                assert _lease_count(context) == baseline

                asyncio.run(sender.send().messages(b"old-1", b"old-2").submit())
                assert receiver.recv_retained_into(retained)
                assert _lease_count(context) == baseline + 2
                asyncio.run(sender.send().message(b"replacement").submit())
                assert receiver.recv_retained_into(retained)
                assert retained.to_bytes_list() == [b"replacement"]
                assert _lease_count(context) == baseline + 1
                assert not receiver.recv_retained_into(
                    retained, flags=zlink.RecvFlags.DONT_WAIT
                )
                assert retained.to_bytes_list() == []
                assert _lease_count(context) == baseline

                asyncio.run(sender.send().message(b"gc-fallback").submit())
                assert receiver.recv_retained_into(retained)
                assert _lease_count(context) == baseline + 1
                bare_part = retained.first_part()
                del retained
                gc.collect()
                assert _lease_count(context) == baseline
                assert bare_part.to_bytes() == b"gc-fallback"
                bare_part.close()


def test_retained_router_dealer_and_subscribe_preserve_aggregate_metadata():
    with zlink.create_context() as context:
        baseline = _lease_count(context)
        with zlink.create_dealer_socket(context) as dealer:
            with zlink.create_router_socket(context) as router:
                endpoint = _endpoint("retained-router")
                router.bind(endpoint)
                dealer.connect(endpoint)

                async def exchange():
                    pending = asyncio.create_task(
                        dealer.request().message(b"request").submit()
                    )
                    await asyncio.sleep(0)

                    request = zlink.create_received()
                    assert router.recv_retained_into(request)
                    peer_rid = request.routing_id
                    assert peer_rid is not None
                    assert isinstance(request.request_seq, int)
                    assert request.to_bytes_list() == [b"request"]
                    assert _lease_count(context) == baseline + 1
                    with request:
                        request.reply().message(b"reply").submit()
                    assert _lease_count(context) == baseline

                    parts = await pending
                    try:
                        assert [part.to_bytes() for part in parts] == [b"reply"]
                    finally:
                        for part in parts:
                            part.close()

                    # Single part only: a >1-part ROUTER record through
                    # `zlink_send_async` currently aborts the process with
                    # `Assertion failed: !_more_out
                    # (core/src/runtime/sockets/router/router_send_path.cpp:215)`
                    # on this Core 0.13.0 build — reproduced and reported in
                    # doc/perf/perf/bindings-0.12.0/log/
                    # 2026-08-24-python-realignment.md. Out of scope here
                    # (bindings/python cannot patch core/); this keeps the
                    # retained-credit accounting coverage for the
                    # ROUTER->DEALER direction alive with a shape the async
                    # send path can currently carry.
                    await router.send(peer_rid).message(b"dealer-1").submit()
                    dealer_received = zlink.create_received()
                    assert dealer.recv_retained_into(dealer_received)
                    assert dealer_received.request_seq is None
                    assert dealer_received.to_bytes_list() == [b"dealer-1"]
                    assert _lease_count(context) == baseline + 1
                    dealer_received.close()
                    assert _lease_count(context) == baseline

                asyncio.run(exchange())

        with zlink.create_pub_socket(context) as publisher:
            with zlink.create_sub_socket(context) as subscriber:
                subscriber.set_subscription("retained.topic")
                endpoint = _endpoint("retained-subscribe")
                publisher.bind(endpoint)
                subscriber.connect(endpoint)
                publisher.publish("retained.topic").messages(
                    b"topic-1", b"topic-2"
                ).submit()
                topic_message = zlink.create_topic_message()
                assert subscriber.subscribe_retained_into(topic_message)
                assert topic_message.topic == "retained.topic"
                assert topic_message.to_bytes_list() == [b"topic-1", b"topic-2"]
                assert _lease_count(context) == baseline + 2
                topic_message.close()
                assert _lease_count(context) == baseline
