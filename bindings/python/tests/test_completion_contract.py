import asyncio
import copy
import ctypes
import errno
import pickle
import socket as py_socket
import struct
import time
import uuid
from unittest.mock import patch

import pytest
import zlink
from zlink._native.ffi import (
    ZLINK_COMPLETION_REQUEST,
    ZLINK_COMPLETION_WRITABLE,
    ZLINK_DONTWAIT,
    ZLINK_SEND_ADMITTED,
    ZLINK_SEND_TERMINAL,
    ZlinkCompletion,
    ZlinkMsg,
)
from zlink._runtime.eventing.poller import NativePoller
from zlink._runtime.messaging.routed_async import (
    CompletionOwner,
    _CompletionEntry,
    _RequestEntry,
    _SendEntry,
)
from zlink._runtime.sockets.socket_base_impl import RouterSocket
from zlink.contracts.messaging.received import _reply_token_from_native


class _CompletionCloser:
    def __init__(self):
        self.closed = 0

    def zlink_completion_close(self, _completion):
        self.closed += 1


def _completion(
    kind,
    *,
    completion_id=0,
    request_result=0,
    context=None,
    peer_rid=None,
    send_result=ZLINK_SEND_ADMITTED,
    terminal_errno=0,
):
    value = ZlinkCompletion()
    value.struct_size = ctypes.sizeof(ZlinkCompletion)
    value.kind = kind
    value.completion_id = completion_id
    value.user_context = context
    if peer_rid is not None:
        peer_rid = bytes(peer_rid)
        value.peer_rid.size = len(peer_rid)
        value.peer_rid.data[: len(peer_rid)] = peer_rid
    value.send_result = send_result
    value.send_terminal_errno = terminal_errno
    value.request_result = request_result
    return value


def _native_routing_id_bytes(native):
    size = int(native.size)
    return None if size == 0 else bytes(native.data[:size])


async def _next_event_loop_turn():
    loop = asyncio.get_running_loop()
    ready = loop.create_future()
    loop.call_soon(ready.set_result, None)
    await ready


def test_reply_token_is_private_immutable_and_owner_bound():
    with pytest.raises(TypeError):
        zlink.ReplyToken()

    owner = object()
    token = _reply_token_from_native(owner, 41)
    same = _reply_token_from_native(owner, 41)
    other_owner = _reply_token_from_native(object(), 41)
    assert token == same
    assert hash(token) == hash(same)
    assert token != other_owner
    assert repr(token) == "ReplyToken()"
    assert copy.copy(token) is token
    assert copy.deepcopy(token) is token
    with pytest.raises(TypeError):
        pickle.dumps(token)
    with pytest.raises(AttributeError):
        token._value = 42
    with pytest.raises(TypeError):
        int(token)


def test_reply_rejects_another_socket_owner_before_native_entry():
    router = object.__new__(RouterSocket)
    router._reply_owner = object()
    wrong = _reply_token_from_native(object(), 7)
    with pytest.raises(zlink.SubmitError) as raised:
        router.reply(zlink.RoutingId.from_(b"peer"), wrong)
    assert raised.value.result == zlink.SubmitResult.INVALID_ARGUMENT


def test_stream_packet_resets_closes_and_reuses_storage():
    class Part:
        def __init__(self):
            self.closed = 0

        def close(self):
            self.closed += 1

    packet = zlink.StreamPacket()
    first_header = Part()
    first_body = Part()
    packet._begin_receive()
    packet._finish_receive(zlink.RoutingId.from_(b"peer"), first_header, first_body)
    assert not packet.is_empty

    packet._begin_receive()
    assert packet.is_empty
    assert first_header.closed == 1
    assert first_body.closed == 1
    packet._finish_receive()

    packet._begin_receive()
    with pytest.raises(zlink.RecvError) as raised:
        packet._begin_receive()
    assert raised.value.result == zlink.RecvResult.BUSY
    packet._finish_receive()
    packet.close()
    assert packet.is_empty


def test_stream_packet_receive_success_no_data_reset_and_reuse():
    probe = py_socket.socket(py_socket.AF_INET, py_socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    endpoint = f"tcp://127.0.0.1:{port}"

    with zlink.create_context() as context:
        with zlink.create_stream_socket(context) as stream:
            assert stream.stream_options.recv_mode is zlink.StreamRecvMode.UNSPECIFIED
            with pytest.raises(zlink.ConfigError):
                stream.stream_options.recv_mode = zlink.StreamRecvMode.UNSPECIFIED
            stream.stream_options.recv_mode = zlink.StreamRecvMode.PACKET
            assert stream.stream_options.recv_mode is zlink.StreamRecvMode.PACKET
            stream.bind(endpoint)
            with py_socket.create_connection(("127.0.0.1", port), timeout=3.0) as peer:
                packet = zlink.StreamPacket()
                for expected in (b"first", b"second"):
                    peer.sendall(struct.pack("!HI", 0, len(expected)) + expected)
                    deadline = time.monotonic() + 3.0
                    while not stream.recv_packet_into(
                        packet, flags=zlink.RecvFlags.DONT_WAIT
                    ):
                        if time.monotonic() >= deadline:
                            raise AssertionError("timed out waiting for STREAM packet")
                        time.sleep(0.001)
                    assert packet.header.to_bytes() == b""
                    assert packet.body.to_bytes() == expected
                    assert packet.routing_id is not None
                    assert not stream.recv_packet_into(
                        packet, flags=zlink.RecvFlags.DONT_WAIT
                    )
                    assert packet.is_empty
                packet.close()


def test_request_completion_capture_before_submit_return_joins_once():
    async def exercise():
        entry = _CompletionEntry(ZLINK_COMPLETION_REQUEST, asyncio.get_running_loop())
        closer = _CompletionCloser()
        with patch("zlink._runtime.messaging.routed_async.lib", return_value=closer):
            entry.capture(
                _completion(
                    ZLINK_COMPLETION_REQUEST,
                    completion_id=17,
                    context=entry.context,
                )
            )
            assert not entry.future.done()
            entry.publish(17)
            await entry.wait_async()
            assert entry.future.done()
            assert closer.closed == 1

    asyncio.run(exercise())


def test_cancelled_wait_keeps_native_operation_and_late_completion_cleans_once():
    class FakeMessage:
        created = []

        def __new__(cls):
            value = super().__new__(cls)
            value.closed = 0
            cls.created.append(value)
            return value

        def close(self):
            self.closed += 1

    async def exercise():
        entry = _CompletionEntry(ZLINK_COMPLETION_REQUEST, asyncio.get_running_loop())
        entry.publish(23)
        waiter = asyncio.create_task(entry.wait_async())
        await asyncio.sleep(0)
        waiter.cancel()
        with pytest.raises(asyncio.CancelledError):
            await waiter

        completion = _completion(
            ZLINK_COMPLETION_REQUEST,
            completion_id=23,
            context=entry.context,
        )
        parts = (ZlinkMsg * 1)()
        completion.reply_part_count = 1
        completion.reply_parts = parts
        closer = _CompletionCloser()

        def clone(_native):
            return object()

        with (
            patch("zlink._runtime.messaging.routed_async.lib", return_value=closer),
            patch("zlink._runtime.messaging.routed_async.Message", FakeMessage),
            patch("zlink._runtime.messaging.routed_async._clone_native_msg", side_effect=clone),
        ):
            entry.capture(completion)
        await asyncio.sleep(0)
        assert closer.closed == 1
        assert len(FakeMessage.created) == 1
        assert FakeMessage.created[0].closed == 1
        assert not entry.future.done()
        assert not entry.future.cancelled()

    asyncio.run(exercise())


def test_non_ok_request_completion_is_typed_error_without_payload():
    entry = _CompletionEntry(ZLINK_COMPLETION_REQUEST)
    closer = _CompletionCloser()
    with patch("zlink._runtime.messaging.routed_async.lib", return_value=closer):
        entry.capture(
            _completion(
                ZLINK_COMPLETION_REQUEST,
                completion_id=31,
                context=entry.context,
                request_result=int(zlink.RequestResult.TIMED_OUT),
            )
        )
    entry.publish(31)
    with pytest.raises(zlink.RequestError) as raised:
        entry.wait_request()
    assert raised.value.result == zlink.RequestResult.TIMED_OUT
    assert closer.closed == 1


@pytest.mark.parametrize("mismatch", ("token", "context", "rid"))
def test_writable_completion_rejects_mismatched_send_correlation(mismatch):
    target = b"expected-route"
    socket = type("Socket", (), {"_handle": 1})()
    owner = CompletionOwner(socket)
    entry = _SendEntry(None, target, [])
    assert entry.await_writable(71)
    completion = _completion(
        ZLINK_COMPLETION_WRITABLE,
        completion_id=71,
        context=entry.context,
        peer_rid=target,
    )
    if mismatch == "token":
        completion.completion_id = 72
    elif mismatch == "context":
        completion.user_context = entry.context + 1
    else:
        replacement = b"different-route"
        completion.peer_rid.size = len(replacement)
        completion.peer_rid.data[: len(replacement)] = replacement

    closer = _CompletionCloser()
    with (
        patch("zlink._runtime.messaging.routed_async.lib", return_value=closer),
        patch.object(owner, "_attempt_send") as retry,
    ):
        owner._capture_writable(entry, completion)

    retry.assert_not_called()
    assert entry.settled
    assert isinstance(entry._error, zlink.SubmitError)
    assert entry._error.result == zlink.SubmitResult.INTERNAL_ERROR
    assert closer.closed == 1


@pytest.mark.parametrize(
    ("native_errno", "expected_result"),
    (
        (errno.ENOENT, zlink.SubmitResult.NOT_FOUND),
        (
            getattr(errno, "ESHUTDOWN", errno.ECANCELED),
            zlink.SubmitResult.TERMINATED,
        ),
        (int(zlink.ErrorCode.ETERM), zlink.SubmitResult.TERMINATED),
    ),
)
def test_terminal_writable_is_typed_and_never_retried(
    native_errno, expected_result
):
    target = b"terminal-route"
    socket = type("Socket", (), {"_handle": 1})()
    owner = CompletionOwner(socket)
    entry = _SendEntry(None, target, [])
    assert entry.await_writable(81)
    completion = _completion(
        ZLINK_COMPLETION_WRITABLE,
        completion_id=81,
        context=entry.context,
        peer_rid=target,
        send_result=ZLINK_SEND_TERMINAL,
        terminal_errno=native_errno,
    )

    closer = _CompletionCloser()
    with (
        patch("zlink._runtime.messaging.routed_async.lib", return_value=closer),
        patch.object(owner, "_attempt_send") as retry,
    ):
        assert not owner._capture_writable(entry, completion)

    retry.assert_not_called()
    assert entry.settled
    assert entry._error.result == expected_result
    assert entry._error.native_errno == native_errno
    assert closer.closed == 1


@pytest.mark.parametrize(
    ("native_errno", "expected_result"),
    (
        (errno.ENOENT, zlink.SubmitResult.NOT_FOUND),
        (
            getattr(errno, "ESHUTDOWN", errno.ECANCELED),
            zlink.SubmitResult.TERMINATED,
        ),
        (int(zlink.ErrorCode.ETERM), zlink.SubmitResult.TERMINATED),
    ),
)
def test_request_terminal_writable_is_typed_and_never_retried(
    native_errno, expected_result
):
    target = b"terminal-request-route"
    socket = type("Socket", (), {"_handle": 1})()
    owner = CompletionOwner(socket)
    entry = _RequestEntry(None, 1000)
    entry.target = target
    entry.payload = []
    assert entry.await_writable(91)
    completion = _completion(
        ZLINK_COMPLETION_WRITABLE,
        completion_id=91,
        context=entry.context,
        peer_rid=target,
        send_result=ZLINK_SEND_TERMINAL,
        terminal_errno=native_errno,
    )

    closer = _CompletionCloser()
    with (
        patch("zlink._runtime.messaging.routed_async.lib", return_value=closer),
        patch.object(owner, "_attempt_request") as retry,
    ):
        assert not owner._capture_writable(entry, completion)

    retry.assert_not_called()
    assert entry.settled
    assert isinstance(entry._error, zlink.SubmitError)
    assert entry._error.result == expected_result
    assert entry._error.native_errno == native_errno
    assert closer.closed == 1


def test_send_entry_releases_retained_native_snapshot_exactly_once():
    class NativeCloser:
        def __init__(self):
            self.closed = 0

        def zlink_msg_close(self, _message):
            self.closed += 1

    closer = NativeCloser()
    entry = _SendEntry(None, None, [ZlinkMsg(), ZlinkMsg()])
    with patch("zlink._runtime.messaging.routed_async.lib", return_value=closer):
        entry.fail(
            zlink.SubmitError(
                zlink.SubmitResult.TERMINATED,
                getattr(errno, "ESHUTDOWN", errno.ECANCELED),
            )
        )
        entry.shutdown()
    assert closer.closed == 2


def test_public_routed_send_without_route_has_no_wait_token():
    async def exercise():
        with zlink.create_context() as context:
            with zlink.create_router_socket(context) as router:
                router.options.linger_ms = 0
                owner = router._completion_owner
                native_submit = owner._submit_parts
                submissions = []

                def observe(*args, **kwargs):
                    result = native_submit(*args, **kwargs)
                    submissions.append(result)
                    return result

                missing = zlink.RoutingId.from_(b"missing-route")
                with (
                    patch.object(owner, "_submit_parts", side_effect=observe),
                    pytest.raises(zlink.SubmitError) as raised,
                ):
                    await router.send(missing).message(b"payload").submit()

                assert raised.value.result == zlink.SubmitResult.NOT_CONNECTED
                assert raised.value.native_errno == errno.EHOSTUNREACH
                assert submissions == [
                    (int(zlink.SubmitResult.NOT_CONNECTED), errno.EHOSTUNREACH, 0)
                ]
                assert not owner._entries
                assert not owner._entries_by_id

    asyncio.run(exercise())


def test_public_managed_routed_send_retries_after_exact_writable_completion():
    async def exercise():
        context = zlink.create_context()
        context.options.auto_hwm_enabled = False
        router = zlink.create_router_socket(context)
        dealer = zlink.create_dealer_socket(context)
        poller = zlink.create_poller()
        tasks = []
        try:
            router.options.linger_ms = 0
            dealer.options.linger_ms = 0
            router.options.immediate = True
            router.options.send_high_water_mark = 512
            dealer.options.receive_high_water_mark = 512
            router.router_options.mandatory = True

            peer = zlink.RoutingId.from_(
                f"python-managed-peer-{uuid.uuid4().hex}"
            )
            dealer.set_routing_id(peer)
            endpoint = f"inproc://python-managed-writable-{uuid.uuid4().hex}"
            router.bind(endpoint)
            dealer.connect(endpoint)

            # Blocking traffic is the pipe-attach barrier; no timing delay is
            # needed before the DONTWAIT HWM exercise.
            dealer.send().message(b"route-ready").submit_sync()
            route_ready = zlink.create_received()
            try:
                assert router.recv_into(route_ready)
                assert route_ready.routing_id == peer
                target = route_ready.routing_id
            finally:
                route_ready.close()

            events = zlink.create_poll_events(1)
            poller.add_socket(
                router,
                zlink.PollEventFlag.POLLOUT
                | zlink.PollEventFlag.POLLCOMPLETION,
                79,
            )

            owner = router._completion_owner
            native_submit = owner._submit_parts
            native_capture = owner._capture_writable
            submit_records = []
            writable_records = []

            def observe_submit(
                submit_target,
                native_parts,
                flags,
                entry=None,
                timeout_ms=None,
            ):
                result = native_submit(
                    submit_target,
                    native_parts,
                    flags,
                    entry,
                    timeout_ms,
                )
                submit_records.append(
                    {
                        "context": 0 if entry is None else entry.context,
                        "target": (
                            None
                            if submit_target is None
                            else bytes(submit_target)
                        ),
                        "result": result[0],
                        "errno": result[1],
                        "completion_id": result[2],
                        "flags": int(flags),
                    }
                )
                return result

            def observe_writable(entry, completion):
                writable_records.append(
                    {
                        "kind": int(completion.kind),
                        "completion_id": int(completion.completion_id),
                        "context": int(completion.user_context or 0),
                        "target": _native_routing_id_bytes(completion.peer_rid),
                        "send_result": int(completion.send_result),
                        "terminal_errno": int(completion.send_terminal_errno),
                    }
                )
                return native_capture(entry, completion)

            accepted = []
            pending = None
            blocked_source = None
            blocked_payload = None
            with (
                patch.object(owner, "_submit_parts", side_effect=observe_submit),
                patch.object(
                    owner, "_capture_writable", side_effect=observe_writable
                ),
            ):
                for index in range(512):
                    payload = bytearray(
                        index.to_bytes(4, "little") + b"x" * 60
                    )
                    expected = bytes(payload)
                    task = asyncio.create_task(
                        router.send(target).message(payload).submit()
                    )
                    tasks.append(task)
                    await _next_event_loop_turn()
                    if task.done():
                        await task
                        accepted.append(expected)
                        continue
                    pending = task
                    blocked_source = payload
                    blocked_payload = expected
                    break

                assert accepted
                assert pending is not None, "send HWM did not backpressure"
                blocked = next(
                    record
                    for record in submit_records
                    if record["result"]
                    == int(zlink.SubmitResult.BACKPRESSURED)
                )
                assert blocked["errno"] == errno.EAGAIN
                assert blocked["completion_id"] != 0
                assert blocked["context"] != 0
                assert blocked["target"] == target.to_bytes()
                assert blocked["flags"] == ZLINK_DONTWAIT

                # The managed operation owns an immutable packet snapshot while
                # waiting; caller mutation cannot alter the retry payload.
                blocked_source[:] = b"z" * len(blocked_source)
                assert poller.wait(events, 0) == 0

                for expected in accepted:
                    received = zlink.create_received()
                    try:
                        assert dealer.recv_into(received)
                        assert received.to_bytes_list() == [expected]
                    finally:
                        received.close()

                assert poller.wait(events, 5000) == 1
                assert events.slot(0) == 79
                assert events.has_event(0, zlink.PollEventFlag.POLLOUT)
                assert not events.has_event(
                    0, zlink.PollEventFlag.POLLCOMPLETION
                )
                await pending

                assert len(writable_records) == 1
                writable = writable_records[0]
                assert writable["kind"] == int(zlink.CompletionKind.WRITABLE)
                assert writable["completion_id"] == blocked["completion_id"]
                assert writable["context"] == blocked["context"]
                assert writable["target"] == blocked["target"]
                assert writable["send_result"] == ZLINK_SEND_ADMITTED
                assert writable["terminal_errno"] == 0

                blocked_index = submit_records.index(blocked)
                retry_records = submit_records[blocked_index + 1 :]
                assert len(retry_records) == 1
                retry = retry_records[0]
                assert retry["context"] == blocked["context"]
                assert retry["target"] == blocked["target"]
                assert retry["result"] == int(zlink.SubmitResult.OK)
                assert retry["errno"] == 0
                assert retry["completion_id"] == 0
                assert retry["flags"] == ZLINK_DONTWAIT

                retried = zlink.create_received()
                try:
                    assert dealer.recv_into(retried)
                    assert retried.to_bytes_list() == [blocked_payload]
                finally:
                    retried.close()
                duplicate = zlink.create_received()
                try:
                    assert not dealer.recv_into(
                        duplicate, flags=zlink.RecvFlags.DONT_WAIT
                    )
                finally:
                    duplicate.close()
        finally:
            for task in tasks:
                if not task.done():
                    task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            poller.close()
            dealer.close()
            router.close()
            context.close()

    asyncio.run(exercise())


def test_public_poller_atomically_takes_and_returns_completion_owner():
    class FakeOwner:
        def __init__(self):
            self.public = []
            self.runtime = []

        def transfer_to_public(self, poller):
            self.public.append(poller)

        def transfer_to_runtime(self, poller):
            self.runtime.append(poller)

    class FakeLib:
        def zlink_poller_add(self, *_args):
            return 0

        def zlink_poller_remove(self, *_args):
            return 0

        def zlink_errno(self):
            return 0

    owner = FakeOwner()
    socket = type("Socket", (), {"_handle": 9, "_completion_owner": owner})()
    poller = NativePoller.__new__(NativePoller)
    poller._handle = 4
    poller._socket_registrations = {}
    with patch("zlink._runtime.eventing.poller.lib", return_value=FakeLib()):
        poller.add_socket(socket, zlink.PollEventFlag.POLLCOMPLETION, 1)
        assert owner.public == [poller]
        poller.remove_socket(socket)
        assert owner.runtime == [poller]


def test_public_send_request_reply_and_publish_shapes_are_flag_free():
    with zlink.create_context() as context:
        pair = zlink.create_pair_socket(context)
        dealer = zlink.create_dealer_socket(context)
        router = zlink.create_router_socket(context)
        publisher = zlink.create_pub_socket(context)
        try:
            send = pair.send().message(b"payload")
            request = dealer.request().message(b"request")
            publish = publisher.publish("topic").message(b"event")
            assert not hasattr(send, "flags")
            assert hasattr(send, "submit_sync")
            assert not hasattr(request, "flags")
            assert hasattr(request, "timeout")
            assert hasattr(publish, "flags")
            send.submit().close()
            request.submit().close()
        finally:
            publisher.close()
            router.close()
            dealer.close()
            pair.close()
