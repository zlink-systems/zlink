"""HWM-managed send and request completion under the 0.13.1 contract.

Bindings own zero threads, queues, or retry. PAIR send and DEALER/ROUTER
routed send are admitted by `zlink_send_async` and completed by Core's
`zlink_send_complete_handler` — never by a binding-owned park queue,
WRITABLE-callback retry, deadline timer, or dispatcher thread. request
completion is purely the Core reply callback resolving the awaitable. Core
retries every accepted send; only an initial rejection at the Core pending
operation bound surfaces immediately for application policy.
"""

import asyncio
import inspect
import threading
import time
import uuid

import zlink


def _endpoint(label):
    return f"inproc://python-routed-async-{label}-{uuid.uuid4()}"


def _thread_names():
    return {thread.name for thread in threading.enumerate()}


async def _send_when_connected(socket, payload, routing_id=None):
    for _ in range(200):
        try:
            operation = socket.send() if routing_id is None else socket.send(routing_id)
            return await operation.message(payload).submit()
        except zlink.SubmitError as error:
            if error.result not in (
                zlink.SubmitResult.NOT_CONNECTED,
                zlink.SubmitResult.NOT_FOUND,
                zlink.SubmitResult.BACKPRESSURED,
            ):
                raise
            await asyncio.sleep(0.001)
    raise AssertionError("routed target did not connect")


def test_hwm_send_wait_is_a_coroutine_and_owns_no_binding_thread():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    dealer.options.send_high_water_mark = 2048
                    endpoint = _endpoint("event-loop")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    payload = b"x" * 65536
                    await _send_when_connected(dealer, payload)
                    before = _thread_names()

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
                    # No binding-owned thread appears while a send waits on
                    # Core admission — completion is Core's send-completion
                    # notification, not a park queue this binding services.
                    assert _thread_names() == before

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


def test_pair_send_is_also_hwm_managed_and_coroutine_driven():
    """PAIR send is ASYNC-classified exactly like DEALER/ROUTER routed send
    (분류 원칙, async-coroutine-policy.ko.md) — same `zlink_send_async` +
    `zlink_send_complete_handler` completion surface, no binding thread."""

    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_pair_socket(context) as left:
                with zlink.create_pair_socket(context) as right:
                    left.options.linger_ms = 0
                    right.options.linger_ms = 0
                    left.options.send_high_water_mark = 2048
                    endpoint = _endpoint("pair-hwm")
                    right.bind(endpoint)
                    left.connect(endpoint)

                    payload = b"y" * 65536
                    for _ in range(200):
                        try:
                            await left.send().message(payload).submit()
                            break
                        except zlink.SubmitError as error:
                            if error.result not in (
                                zlink.SubmitResult.NOT_CONNECTED,
                                zlink.SubmitResult.BACKPRESSURED,
                            ):
                                raise
                            await asyncio.sleep(0.001)

                    before = _thread_names()
                    pending = asyncio.create_task(
                        left.send().message(payload).submit()
                    )
                    await asyncio.sleep(0.03)
                    assert not pending.done()
                    assert _thread_names() == before

                    received = zlink.create_received()
                    assert right.recv_into(received)
                    received.close()
                    assert await asyncio.wait_for(pending, 2) is None
                    assert right.recv_into(received)
                    received.close()

    asyncio.run(scenario())


def test_sync_blocking_terminal_admits_and_returns_none():
    with zlink.create_context() as context:
        with zlink.create_pair_socket(context) as left:
            with zlink.create_pair_socket(context) as right:
                left.options.linger_ms = 0
                right.options.linger_ms = 0
                endpoint = _endpoint("sync-blocking")
                right.bind(endpoint)
                left.connect(endpoint)

                for _ in range(200):
                    try:
                        result = left.send().message(b"sync").submit_sync()
                        break
                    except zlink.SubmitError as error:
                        if error.result != zlink.SubmitResult.NOT_CONNECTED:
                            raise
                        time.sleep(0.001)
                else:
                    raise AssertionError("PAIR target did not connect")

                assert result is None
                received = zlink.create_received()
                assert right.recv_into(received)
                assert received.to_bytes_list() == [b"sync"]
                received.close()


def test_sync_dontwait_immediately_raises_when_hwm_is_full():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    dealer.options.send_high_water_mark = 2048
                    endpoint = _endpoint("sync-dontwait")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    payload = b"z" * 65536
                    await _send_when_connected(dealer, payload)
                    started = time.monotonic()
                    try:
                        dealer.send().message(payload).submit_sync(
                            flags=zlink.SendFlags.DONT_WAIT
                        )
                    except zlink.SubmitError as error:
                        assert error.result == zlink.SubmitResult.BACKPRESSURED
                    else:
                        raise AssertionError("full HWM did not backpressure")
                    assert time.monotonic() - started < 0.1

                    received = zlink.create_received()
                    assert router.recv_into(received)
                    assert (
                        received.send()
                        .message(b"received-send")
                        .submit_sync()
                        is None
                    )
                    received.close()
                    assert dealer.recv_into(received)
                    assert received.to_bytes_list() == [b"received-send"]
                    received.close()

    asyncio.run(scenario())


def test_async_submit_remains_a_coroutine_terminal():
    operation = zlink.RoutedSendOp.submit_sync
    signature = inspect.signature(operation)
    assert str(signature) == "(self, *, flags=0) -> None"

    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_pair_socket(context) as left:
                coroutine = left.send().message(b"async-regression").submit()
                assert inspect.iscoroutine(coroutine)
                coroutine.close()

    asyncio.run(scenario())


def test_request_sync_return_and_callback_terminals():
    with zlink.create_context() as context:
        with zlink.create_dealer_socket(context) as dealer:
            with zlink.create_router_socket(context) as router:
                dealer.options.linger_ms = 0
                router.options.linger_ms = 0
                endpoint = _endpoint("request-sync-terminals")
                router.bind(endpoint)
                dealer.connect(endpoint)

                received = zlink.create_received()

                def reply_once(expected, reply):
                    for _ in range(200):
                        try:
                            if router.recv_into(received):
                                break
                        except zlink.RecvError as error:
                            if error.result != zlink.RecvResult.NO_DATA:
                                raise
                        time.sleep(0.001)
                    else:
                        raise AssertionError("request did not reach responder")
                    assert received.to_bytes_list() == [expected]
                    received.reply().message(reply).submit()
                    received.close()

                reply_thread = threading.Thread(
                    target=reply_once, args=(b"sync-return", b"return-reply")
                )
                reply_thread.start()
                reply = (
                    dealer.request()
                    .message(b"sync-return")
                    .timeout(1)
                    .submit_sync(flags=zlink.SendFlags.NONE)
                )
                reply_thread.join(timeout=2)
                assert not reply_thread.is_alive()
                try:
                    assert [part.to_bytes() for part in reply] == [b"return-reply"]
                finally:
                    for part in reply:
                        part.close()

                callback_done = threading.Event()
                callback_result = {}

                def on_reply(parts, error):
                    callback_result["error"] = error
                    callback_result["parts"] = parts
                    callback_done.set()

                assert (
                    dealer.request()
                    .message(b"sync-callback")
                    .timeout(1)
                    .submit_sync(flags=zlink.SendFlags.NONE, callback=on_reply)
                    is None
                )
                reply_once(b"sync-callback", b"callback-reply")
                assert callback_done.wait(2)
                assert callback_result["error"] is None
                parts = callback_result["parts"]
                try:
                    assert [part.to_bytes() for part in parts] == [b"callback-reply"]
                finally:
                    for part in parts:
                        part.close()


def test_request_sync_callback_dontwait_surfaces_admission_backpressure():
    with zlink.create_context() as context:
        with zlink.create_dealer_socket(context) as dealer:
            with zlink.create_router_socket(context) as router:
                dealer.options.linger_ms = 0
                router.options.linger_ms = 0
                dealer.options.send_high_water_mark = 2048
                endpoint = _endpoint("request-sync-callback-backpressure")
                router.bind(endpoint)
                dealer.connect(endpoint)

                payload = b"q" * 65536
                callbacks = []

                def on_reply(parts, error):
                    callbacks.append((parts, error))
                    if parts is not None:
                        for part in parts:
                            part.close()

                admitted = 0
                for _ in range(200):
                    try:
                        (
                            dealer.request()
                            .message(payload)
                            .timeout(0.05)
                            .submit_sync(
                                flags=zlink.SendFlags.DONT_WAIT,
                                callback=on_reply,
                            )
                        )
                        admitted += 1
                    except zlink.SubmitError as error:
                        if error.result in (
                            zlink.SubmitResult.NOT_CONNECTED,
                            zlink.SubmitResult.NOT_FOUND,
                        ):
                            time.sleep(0.001)
                            continue
                        assert error.result == zlink.SubmitResult.BACKPRESSURED
                        break
                else:
                    raise AssertionError("request admission did not backpressure")
                assert admitted > 0


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


def test_router_request_uses_the_same_exact_target_reply_driven_path():
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


def test_request_reply_timeout_is_core_driven_with_no_binding_thread():
    """The request timeout is `ZLINK_REQUEST_TIMED_OUT` — a Core deadline,
    not a Python timer (`_runtime/eventing/timer.py` is the existing
    Core-timer precedent this mirrors)."""

    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    endpoint = _endpoint("request-timeout")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    await _send_when_connected(dealer, b"probe")
                    received = zlink.create_received()
                    assert router.recv_into(received)
                    received.close()

                    before = _thread_names()
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
                    else:
                        raise AssertionError("request did not time out")
                    assert time.monotonic() - started < 1
                    assert _thread_names() == before

    asyncio.run(scenario())


def test_dealer_recv_reuse_preserves_typed_request_sequence():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_router_socket(context) as router:
                with zlink.create_dealer_socket(context) as dealer:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    dealer.set_routing_id(b"dealer-recv-request-seq")
                    endpoint = _endpoint("dealer-recv-request-seq")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    await _send_when_connected(dealer, b"ready")
                    router_received = zlink.create_received()
                    assert router.recv_into(router_received)
                    router_received.close()

                    received = zlink.create_received()
                    await _send_when_connected(
                        router, b"ordinary", b"dealer-recv-request-seq"
                    )
                    assert dealer.recv_into(received)
                    assert received.to_bytes_list() == [b"ordinary"]
                    assert received.request_seq is None

                    pending = asyncio.create_task(
                        router.request(b"dealer-recv-request-seq")
                        .message(b"typed-request")
                        .timeout(0.05)
                        .submit()
                    )
                    await asyncio.sleep(0)
                    assert dealer.recv_into(received)
                    assert received.to_bytes_list() == [b"typed-request"]
                    assert isinstance(received.request_seq, int)
                    received.close()

                    try:
                        await pending
                    except zlink.RequestError as error:
                        assert error.result == zlink.RequestResult.TIMED_OUT
                    else:
                        raise AssertionError("typed request did not time out")

    asyncio.run(scenario())


def test_send_cancellation_is_terminal_exactly_once():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    dealer.options.send_high_water_mark = 2048
                    endpoint = _endpoint("send-cancel")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    payload = b"x" * 65536
                    await _send_when_connected(dealer, payload)

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

    asyncio.run(scenario())


def test_request_cancellation_is_preserved_and_a_late_reply_is_dropped():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    endpoint = _endpoint("request-cancel")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    await _send_when_connected(dealer, b"probe")
                    received = zlink.create_received()
                    assert router.recv_into(received)
                    received.close()

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
                    # A reply that arrives after cancellation finds no armed
                    # completion and is dropped rather than raising.
                    received.reply().message(b"late-reply").submit()
                    received.close()

                    request = dealer.request().message(b"surface")
                    assert not hasattr(request, "flags")
                    assert not hasattr(request, "submit_async")

    asyncio.run(scenario())
