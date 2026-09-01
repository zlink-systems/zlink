"""HWM-managed send and request completion under the 0.13.1 contract.

Bindings own zero threads, queues, or retry. PAIR send and DEALER/ROUTER/STREAM
routed send are admitted by `zlink_send_async` and completed by Core's
`zlink_send_complete_handler` — never by a binding-owned park queue,
WRITABLE-callback retry, deadline timer, or dispatcher thread. request
completion is purely the Core reply callback resolving the awaitable. Core
retries every accepted send; only an initial rejection at the Core pending
operation bound surfaces immediately for application policy.
"""

import asyncio
import ctypes
import errno
import inspect
import socket
import struct
import threading
import time
import uuid
from types import SimpleNamespace
from unittest.mock import patch

import pytest
import zlink
from zlink._native.ffi import (
    ZLINK_SEND_ADMITTED,
    ZLINK_SEND_TERMINAL,
    ZLINK_SEND_TIMED_OUT,
    ZlinkMsg,
    ZlinkSendAsyncOptions,
    ZlinkSendCompleteEvent,
)
from zlink._runtime.messaging import routed_async as routed_async_runtime


def _endpoint(label):
    return f"inproc://python-routed-async-{label}-{uuid.uuid4()}"


def _thread_names():
    return {thread.name for thread in threading.enumerate()}


def _tcp_endpoint():
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port, f"tcp://127.0.0.1:{port}"


class _SendAsyncStub:
    def __init__(
        self,
        *,
        op_id,
        before_return=None,
        terminal_errno=0,
        before_return_hook=None,
    ):
        self.op_id = op_id
        self.before_return = before_return
        self.terminal_errno = terminal_errno
        self.before_return_hook = before_return_hook
        self.handler = None
        self.token = None
        self.cancelled = []

    def zlink_send_complete_handler(self, _handle, handler, _userdata):
        self.handler = handler
        return int(zlink.HandlerResult.OK)

    def zlink_send_async(
        self, _handle, _parts, _part_count, options_ptr, op_id_out
    ):
        options = ctypes.cast(
            options_ptr, ctypes.POINTER(ZlinkSendAsyncOptions)
        ).contents
        self.token = int(options.userdata)
        if self.before_return is not None:
            self.complete(self.before_return, self.terminal_errno)
        if self.before_return_hook is not None:
            self.before_return_hook()
        ctypes.cast(op_id_out, ctypes.POINTER(ctypes.c_uint64)).contents.value = (
            self.op_id
        )
        return int(zlink.SubmitResult.OK)

    def zlink_send_async_cancel(self, _handle, op_id):
        self.cancelled.append(int(getattr(op_id, "value", op_id)))
        self.complete(ZLINK_SEND_TERMINAL, errno.ECANCELED)
        return int(zlink.SubmitResult.OK)

    def complete(self, result=ZLINK_SEND_ADMITTED, terminal_errno=0):
        event = ZlinkSendCompleteEvent()
        event.op_id = self.op_id
        event.userdata = ctypes.c_void_p(self.token)
        event.result = result
        event.terminal_errno = terminal_errno
        self.handler(None, ctypes.pointer(event), None)


def _patch_send_runtime(native):
    return (
        patch.object(routed_async_runtime, "lib", return_value=native),
        patch.object(
            routed_async_runtime,
            "_materialize_native_parts",
            return_value=[ZlinkMsg()],
        ),
    )


def test_future_completion_finishes_inline_on_event_loop_thread():
    async def scenario():
        loop = asyncio.get_running_loop()
        future = loop.create_future()
        with patch.object(
            loop, "call_soon_threadsafe", wraps=loop.call_soon_threadsafe
        ) as schedule:
            routed_async_runtime._schedule_future(loop, future, "ready")
            assert future.result() == "ready"
            schedule.assert_not_called()

    asyncio.run(scenario())


def test_future_completion_coalesces_cross_thread_loop_wakes():
    async def scenario():
        loop = asyncio.get_running_loop()
        first = loop.create_future()
        second = loop.create_future()
        with patch.object(
            loop, "call_soon_threadsafe", wraps=loop.call_soon_threadsafe
        ) as schedule:
            worker = threading.Thread(
                target=lambda: (
                    routed_async_runtime._schedule_future(loop, first, 1),
                    routed_async_runtime._schedule_future(loop, second, 2),
                )
            )
            worker.start()
            worker.join()
            assert schedule.call_count == 1
            assert await asyncio.gather(first, second) == [1, 2]

    asyncio.run(scenario())


def test_future_completion_batch_continues_after_resolution_and_close_failures():
    class RejectingFuture:
        def __init__(self, loop):
            self._loop = loop

        def done(self):
            return False

        def get_loop(self):
            return self._loop

        def set_result(self, _value):
            raise RuntimeError("resolution failed")

    class CloseProbe:
        def __init__(self, error=None):
            self.close_count = 0
            self._error = error

        def close(self):
            self.close_count += 1
            if self._error is not None:
                raise self._error

    async def scenario():
        loop = asyncio.get_running_loop()
        reported = []
        previous_handler = loop.get_exception_handler()
        loop.set_exception_handler(lambda _loop, context: reported.append(context))
        failing_close = CloseProbe(RuntimeError("close failed"))
        following_close = CloseProbe()
        second = loop.create_future()
        batch = routed_async_runtime._FutureCompletionBatch(loop)
        try:
            worker = threading.Thread(
                target=lambda: (
                    batch.enqueue(
                        RejectingFuture(loop),
                        [failing_close, following_close],
                        False,
                    ),
                    batch.enqueue(second, "second", False),
                )
            )
            worker.start()
            worker.join()

            assert await asyncio.wait_for(second, 1.0) == "second"
        finally:
            loop.set_exception_handler(previous_handler)

        assert failing_close.close_count == 1
        assert following_close.close_count == 1
        assert [str(context["exception"]) for context in reported] == [
            "resolution failed",
            "close failed",
        ]

    asyncio.run(scenario())


def test_immediate_async_send_does_not_allocate_a_future():
    async def scenario():
        native = _SendAsyncStub(op_id=0)
        lib_patch, parts_patch = _patch_send_runtime(native)
        with lib_patch, parts_patch:
            owner = routed_async_runtime.SendCompletionOwner(
                SimpleNamespace(_handle=ctypes.c_void_p(1))
            )
            loop = asyncio.get_running_loop()
            with patch.object(
                loop, "create_future", wraps=loop.create_future
            ) as create_future:
                assert await owner.submit(b"inline") is None
                create_future.assert_not_called()
            assert owner._pending == {}

    asyncio.run(scenario())


@pytest.mark.parametrize(
    ("result", "terminal_errno", "expected_result"),
    [
        (ZLINK_SEND_ADMITTED, 0, None),
        (ZLINK_SEND_TIMED_OUT, 0, zlink.SubmitResult.BACKPRESSURED),
        (ZLINK_SEND_TERMINAL, errno.ECANCELED, zlink.SubmitResult.TERMINATED),
    ],
)
def test_async_send_callback_before_submit_return_is_preserved(
    result, terminal_errno, expected_result
):
    async def scenario():
        native = _SendAsyncStub(
            op_id=17,
            before_return=result,
            terminal_errno=terminal_errno,
        )
        lib_patch, parts_patch = _patch_send_runtime(native)
        with lib_patch, parts_patch:
            owner = routed_async_runtime.SendCompletionOwner(
                SimpleNamespace(_handle=ctypes.c_void_p(2))
            )
            if expected_result is None:
                assert await owner.submit(b"early-success") is None
            else:
                with pytest.raises(zlink.SubmitError) as raised:
                    await owner.submit(b"early-error")
                assert raised.value.result == expected_result
            assert owner._pending == {}

    asyncio.run(scenario())


def test_delayed_async_send_completes_from_callback_thread():
    async def scenario():
        native = _SendAsyncStub(op_id=23)
        lib_patch, parts_patch = _patch_send_runtime(native)
        with lib_patch, parts_patch:
            owner = routed_async_runtime.SendCompletionOwner(
                SimpleNamespace(_handle=ctypes.c_void_p(3))
            )
            pending = asyncio.create_task(owner.submit(b"delayed"))
            await asyncio.sleep(0)
            assert not pending.done()

            callback_thread = threading.Thread(target=native.complete)
            callback_thread.start()
            callback_thread.join()

            assert await asyncio.wait_for(pending, 1) is None
            assert owner._pending == {}

    asyncio.run(scenario())


def test_lazy_async_send_future_keeps_cancel_and_late_callback_exactly_once():
    async def scenario():
        native = _SendAsyncStub(op_id=29)
        lib_patch, parts_patch = _patch_send_runtime(native)
        with lib_patch, parts_patch:
            owner = routed_async_runtime.SendCompletionOwner(
                SimpleNamespace(_handle=ctypes.c_void_p(4))
            )
            pending = asyncio.create_task(owner.submit(b"cancel"))
            await asyncio.sleep(0)
            pending.cancel()
            with pytest.raises(asyncio.CancelledError):
                await pending
            await asyncio.sleep(0)

            assert native.cancelled == [29]
            assert owner._pending == {}
            # Core can race a duplicate/late terminal notification with
            # cancellation; the consumed token makes it a no-op.
            native.complete(ZLINK_SEND_TERMINAL, errno.ECANCELED)
            await asyncio.sleep(0)

    asyncio.run(scenario())


def test_close_drain_before_op_id_publication_completes_lazy_future_once():
    async def scenario():
        native = _SendAsyncStub(op_id=31)
        lib_patch, parts_patch = _patch_send_runtime(native)
        with lib_patch, parts_patch:
            owner = routed_async_runtime.SendCompletionOwner(
                SimpleNamespace(_handle=ctypes.c_void_p(5))
            )
            native.before_return_hook = owner.drain_pending

            with pytest.raises(zlink.SubmitError) as raised:
                await owner.submit(b"close-race")
            assert raised.value.result == zlink.SubmitResult.TERMINATED
            assert owner._pending == {}

            native.complete(ZLINK_SEND_TERMINAL, errno.ECANCELED)
            await asyncio.sleep(0)

    asyncio.run(scenario())


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


def test_stream_send_uses_public_routed_coroutine_terminal():
    async def scenario():
        port, endpoint = _tcp_endpoint()
        with zlink.create_context() as context:
            with zlink.create_stream_socket(context) as stream:
                stream.options.linger_ms = 0
                packet_ready = threading.Event()
                observed_routing_id = []

                def on_packet(routing_id, header, body):
                    assert header.to_bytes() == b""
                    assert body.to_bytes() == b"ping"
                    observed_routing_id.append(bytes(routing_id))
                    packet_ready.set()

                stream.bind(endpoint)
                stream.on_packet(on_packet)
                with socket.create_connection(
                    ("127.0.0.1", port), timeout=3.0
                ) as client:
                    client.sendall(struct.pack("!HI", 0, 4) + b"ping")
                    assert packet_ready.wait(3.0)
                    sync_operation = stream.send(observed_routing_id[0]).message(
                        b"sync"
                    )
                    assert isinstance(sync_operation, zlink.SendOp)
                    assert sync_operation.submit() is True
                    assert client.recv(4) == b"sync"

                    operation = stream.send_async(observed_routing_id[0]).message(
                        b"pong"
                    )
                    assert isinstance(operation, zlink.RoutedSendOp)
                    coroutine = operation.submit()
                    assert inspect.iscoroutine(coroutine)
                    assert await coroutine is None
                    assert client.recv(4) == b"pong"

    asyncio.run(scenario())


def test_routed_multipart_send_uses_core_async_admission():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    endpoint = _endpoint("multipart-async")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    for _ in range(200):
                        try:
                            await dealer.send().messages(b"payload", b"").submit()
                            break
                        except zlink.SubmitError as error:
                            if error.result not in (
                                zlink.SubmitResult.NOT_CONNECTED,
                                zlink.SubmitResult.NOT_FOUND,
                                zlink.SubmitResult.BACKPRESSURED,
                            ):
                                raise
                            await asyncio.sleep(0)
                    else:
                        raise AssertionError("multipart routed send did not connect")

                    received = zlink.create_received()
                    assert router.recv_into(received)
                    assert received.to_bytes_list() == [b"payload", b""]
                    routing_id = bytes(received.routing_id)
                    echo_parts = received.to_bytes_list()
                    received.close()

                    await router.send(routing_id).messages(*echo_parts).submit()
                    assert dealer.recv_into(received)
                    assert received.to_bytes_list() == [b"payload", b""]
                    received.close()

    asyncio.run(scenario())


def test_async_request_reply_preserves_two_application_parts():
    async def scenario():
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    endpoint = _endpoint("multipart-request-reply")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    await _send_when_connected(dealer, b"ready")
                    ready = zlink.create_received()
                    assert router.recv_into(ready)
                    ready.close()

                    pending = asyncio.create_task(
                        dealer.request()
                        .messages(b"payload", b"")
                        .timeout(1.0)
                        .submit()
                    )
                    received = zlink.create_received()
                    for _ in range(200):
                        await asyncio.sleep(0)
                        if router.recv_into(
                            received, flags=zlink.RecvFlags.DONT_WAIT
                        ):
                            break
                    else:
                        raise AssertionError("multipart request did not arrive")

                    assert received.to_bytes_list() == [b"payload", b""]
                    received.reply().messages(b"payload", b"").submit()
                    received.close()

                    reply_parts = await asyncio.wait_for(pending, 2.0)
                    try:
                        assert [part.to_bytes() for part in reply_parts] == [
                            b"payload",
                            b"",
                        ]
                    finally:
                        for part in reply_parts:
                            part.close()

    asyncio.run(scenario())


def test_async_dealer_requests_complete_on_one_public_completion_poller():
    async def scenario():
        request_count = 4
        with zlink.create_context() as context:
            with zlink.create_dealer_socket(context) as dealer:
                with zlink.create_router_socket(context) as router:
                    dealer.options.linger_ms = 0
                    router.options.linger_ms = 0
                    endpoint = _endpoint("request-completion-poller")
                    router.bind(endpoint)
                    dealer.connect(endpoint)

                    await _send_when_connected(dealer, b"ready")
                    ready = zlink.create_received()
                    assert router.recv_into(ready)
                    ready.close()

                    tasks = []
                    completion_counts = [0] * request_count
                    with zlink.create_poller() as poller:
                        poll_events = zlink.create_poll_events(1)
                        poller.add_socket(
                            dealer, zlink.PollEventFlag.POLLCOMPLETION, 41
                        )
                        try:
                            for index in range(request_count):
                                task = asyncio.create_task(
                                    dealer.request()
                                    .messages(b"request", str(index).encode())
                                    .timeout(1.0)
                                    .submit()
                                )
                                task.add_done_callback(
                                    lambda _task, index=index: completion_counts.__setitem__(
                                        index, completion_counts[index] + 1
                                    )
                                )
                                tasks.append(task)

                            received_indexes = set()
                            for _ in range(200):
                                await asyncio.sleep(0)
                                while len(received_indexes) < request_count:
                                    received = zlink.create_received()
                                    if not router.recv_into(
                                        received, flags=zlink.RecvFlags.DONT_WAIT
                                    ):
                                        received.close()
                                        break
                                    try:
                                        request_parts = received.to_bytes_list()
                                        assert request_parts[0] == b"request"
                                        index = int(request_parts[1])
                                        assert index not in received_indexes
                                        received_indexes.add(index)
                                        received.reply().messages(
                                            b"reply", request_parts[1]
                                        ).submit()
                                    finally:
                                        received.close()
                                if len(received_indexes) == request_count:
                                    break
                            assert received_indexes == set(range(request_count))

                            # Registering POLLCOMPLETION transfers callback
                            # dispatch to Poller.wait. Event-loop turns alone
                            # must not finish any request.
                            for _ in range(3):
                                await asyncio.sleep(0)
                            assert all(not task.done() for task in tasks)
                            assert completion_counts == [0] * request_count

                            completion_event_count = 0
                            for _ in range(50):
                                ready_count = poller.wait(poll_events, 20)
                                if ready_count:
                                    completion_event_count += ready_count
                                    assert poll_events.slot(0) == 41
                                    assert poll_events.has_event(
                                        0, zlink.PollEventFlag.POLLCOMPLETION
                                    )
                                await asyncio.sleep(0)
                                if all(task.done() for task in tasks):
                                    break

                            assert completion_event_count > 0
                            replies = await asyncio.gather(*tasks)
                            try:
                                assert [
                                    [part.to_bytes() for part in reply]
                                    for reply in replies
                                ] == [
                                    [b"reply", str(index).encode()]
                                    for index in range(request_count)
                                ]
                            finally:
                                for reply in replies:
                                    for part in reply:
                                        part.close()
                            await asyncio.sleep(0)
                            assert completion_counts == [1] * request_count
                        finally:
                            for task in tasks:
                                if not task.done():
                                    task.cancel()
                            if tasks:
                                await asyncio.gather(*tasks, return_exceptions=True)
                            poller.remove_socket(dealer)
                            assert poller.size() == 0

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
