import asyncio
import errno
import threading
import time
import uuid

import zlink


def _endpoint(label):
    return f"inproc://python-routed-async-{label}-{uuid.uuid4()}"


async def _send_when_connected(socket, payload, routing_id=None):
    for _ in range(200):
        try:
            operation = socket.send() if routing_id is None else socket.send(routing_id)
            return await operation.message(payload).submit()
        except zlink.SubmitError as error:
            if error.result not in (
                zlink.SubmitResult.NOT_CONNECTED,
                zlink.SubmitResult.NOT_FOUND,
            ):
                raise
            await asyncio.sleep(0.001)
    raise AssertionError("routed target did not connect")


def test_hwm_send_wait_is_a_coroutine_and_does_not_occupy_a_worker():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    dealer.options.send_high_water_mark = 2048
                    dealer.options.send_timeout_ms = 3000
                    endpoint = _endpoint("event-loop")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    payload = b"x" * 65536
                    await _send_when_connected(dealer, payload)
                    completion_workers = sum(
                        thread.name == "zlink-request-completion"
                        for thread in threading.enumerate()
                    )
                    pending = asyncio.create_task(
                        dealer.send().message(payload).submit()
                    )

                    heartbeat = False

                    async def tick():
                        nonlocal heartbeat
                        await asyncio.sleep(0.03)
                        heartbeat = True

                    await tick()
                    assert heartbeat
                    assert not pending.done()
                    assert sum(
                        thread.name == "zlink-request-completion"
                        for thread in threading.enumerate()
                    ) == completion_workers

                    received = zlink.create_received()
                    assert router.recv_into(received)
                    received.close()
                    assert await asyncio.wait_for(pending, 2) is None
                    assert router.recv_into(received)
                    received.close()

                    operation = dealer.send()
                    assert isinstance(operation, zlink.RoutedSendOp)
                    assert not isinstance(operation, zlink.SendOp)
                    assert not hasattr(operation, "flags")
                    assert not hasattr(operation, "submit_async")

    asyncio.run(scenario())


def test_exact_target_wait_does_not_block_an_unrelated_routing_id():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_router_socket(context) as router:
                with zlink.create_dealer_socket(context) as dealer_a:
                    with zlink.create_dealer_socket(context) as dealer_b:
                        router.options.linger_ms = 0
                        dealer_a.options.linger_ms = 0
                        dealer_b.options.linger_ms = 0
                        dealer_a.set_routing_id(b"A")
                        dealer_b.set_routing_id(b"B")
                        router.options.send_high_water_mark = 2048
                        router.options.send_timeout_ms = 3000
                        endpoint = _endpoint("target-isolation")
                        router.bind(endpoint)
                        dealer_a.connect(endpoint)
                        dealer_b.connect(endpoint)

                        payload = b"a" * 65536
                        await _send_when_connected(router, payload, b"A")
                        blocked_a = asyncio.create_task(
                            router.send(b"A").message(payload).submit()
                        )
                        await asyncio.sleep(0.03)
                        assert not blocked_a.done()

                        assert (
                            await asyncio.wait_for(
                                _send_when_connected(router, b"b-progress", b"B"),
                                1,
                            )
                            is None
                        )
                        received = zlink.create_received()
                        assert dealer_b.recv_into(received)
                        assert received.to_bytes_list() == [b"b-progress"]
                        received.close()

                        assert dealer_a.recv_into(received)
                        received.close()
                        assert await asyncio.wait_for(blocked_a, 2) is None
                        assert dealer_a.recv_into(received)
                        received.close()

    asyncio.run(scenario())


def test_exact_target_terminal_event_finishes_a_pending_send_once():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                router = zlink.create_router_socket(context)
                dealer.options.linger_ms = 0
                router.options.linger_ms = 0
                dealer.options.send_high_water_mark = 2048
                dealer.options.send_timeout_ms = 3000
                endpoint = _endpoint("target-terminal")
                router.bind(endpoint)
                dealer.connect(endpoint)

                payload = b"x" * 65536
                await _send_when_connected(dealer, payload)
                pending = asyncio.create_task(
                    dealer.send().message(payload).submit()
                )
                await asyncio.sleep(0.03)
                assert not pending.done()
                router.close()
                try:
                    await asyncio.wait_for(pending, 1)
                except zlink.SubmitError as error:
                    assert error.result in (
                        zlink.SubmitResult.NOT_CONNECTED,
                        zlink.SubmitResult.NOT_FOUND,
                        zlink.SubmitResult.TERMINATED,
                    )
                else:
                    raise AssertionError("terminal route event completed as success")

    asyncio.run(scenario())


def test_router_request_uses_the_same_exact_target_coroutine_path():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_router_socket(context) as requester:
                with zlink.create_router_socket(context) as responder:
                    requester.options.linger_ms = 0
                    responder.options.linger_ms = 0
                    requester.set_routing_id(b"requester")
                    responder.set_routing_id(b"responder")
                    endpoint = _endpoint("router-request")
                    responder.bind(endpoint)
                    requester.connect(endpoint)

                    await _send_when_connected(
                        requester, b"route-probe", b"responder"
                    )
                    received = zlink.create_received()
                    assert responder.recv_into(received)
                    received.close()

                    pending = asyncio.create_task(
                        requester.request(b"responder")
                        .messages(b"ping-1", b"ping-2")
                        .submit()
                    )
                    await asyncio.sleep(0)
                    assert responder.recv_into(received)
                    assert received.to_bytes_list() == [b"ping-1", b"ping-2"]
                    received.reply().messages(b"pong-1", b"pong-2").submit()
                    received.close()

                    parts = await pending
                    try:
                        assert [part.to_bytes() for part in parts] == [
                            b"pong-1",
                            b"pong-2",
                        ]
                    finally:
                        for part in parts:
                            part.close()

    asyncio.run(scenario())


def test_request_admission_deadline_and_send_cancellation_are_terminal_once():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    dealer.options.send_high_water_mark = 2048
                    dealer.options.send_timeout_ms = 3000
                    endpoint = _endpoint("deadline-cancel")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    payload = b"x" * 65536
                    await _send_when_connected(dealer, payload)
                    started = time.monotonic()
                    try:
                        await (
                            dealer.request()
                            .message(b"request")
                            .timeout(0.05)
                            .submit()
                        )
                    except zlink.RequestError as error:
                        assert error.result == zlink.RequestResult.TIMED_OUT
                        assert error.native_errno == errno.ETIMEDOUT
                    else:
                        raise AssertionError("request admission did not time out")
                    assert time.monotonic() - started < 1

                    cancelled = asyncio.create_task(
                        dealer.send().message(payload).submit()
                    )
                    await asyncio.sleep(0.02)
                    assert not cancelled.done()
                    cancelled.cancel()
                    try:
                        await cancelled
                    except asyncio.CancelledError:
                        pass
                    else:
                        raise AssertionError("send cancellation was not preserved")

                    received = zlink.create_received()
                    assert router.recv_into(received)
                    received.close()
                    await asyncio.sleep(0.03)
                    assert not router.recv_into(
                        received, flags=zlink.RecvFlags.DONT_WAIT
                    )

                    request_task = asyncio.create_task(
                        dealer.request().message(b"cancel-request").submit()
                    )
                    await asyncio.sleep(0)
                    assert router.recv_into(received)
                    request_task.cancel()
                    try:
                        await request_task
                    except asyncio.CancelledError:
                        pass
                    else:
                        raise AssertionError("request cancellation was not preserved")
                    received.reply().message(b"late-reply").submit()
                    received.close()
                    for _ in range(200):
                        if not dealer._routed_admission.has_request_completions():
                            break
                        await asyncio.sleep(0.001)
                    assert not dealer._routed_admission.has_request_completions()

                    request = dealer.request().message(b"surface")
                    assert not hasattr(request, "flags")
                    assert not hasattr(request, "submit_async")

    asyncio.run(scenario())
