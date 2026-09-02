import asyncio
import copy
import ctypes
import errno
import pickle
import socket as py_socket
import struct
import threading
import time
from unittest.mock import patch

import pytest
import zlink
from zlink._native.ffi import (
    ZLINK_COMPLETION_REQUEST,
    ZLINK_COMPLETION_SEND,
    ZLINK_SEND_ADMITTED,
    ZlinkCompletion,
    ZlinkMsg,
)
from zlink._runtime.eventing.poller import NativePoller
from zlink._runtime.messaging.routed_async import CompletionOwner, _CompletionEntry
from zlink._runtime.sockets.socket_base_impl import RouterSocket
from zlink.contracts.messaging.received import _reply_token_from_native


class _CompletionCloser:
    def __init__(self):
        self.closed = 0

    def zlink_completion_close(self, _completion):
        self.closed += 1


def _completion(kind, *, request_result=0, context=None):
    value = ZlinkCompletion()
    value.struct_size = ctypes.sizeof(ZlinkCompletion)
    value.kind = kind
    value.user_context = context
    value.send_result = ZLINK_SEND_ADMITTED
    value.request_result = request_result
    return value


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


def test_completion_capture_before_submit_return_joins_once():
    async def exercise():
        entry = _CompletionEntry(ZLINK_COMPLETION_SEND, asyncio.get_running_loop())
        closer = _CompletionCloser()
        with patch("zlink._runtime.messaging.routed_async.lib", return_value=closer):
            entry.capture(_completion(ZLINK_COMPLETION_SEND))
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

        completion = _completion(ZLINK_COMPLETION_REQUEST)
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
                request_result=int(zlink.RequestResult.TIMED_OUT),
            )
        )
    entry.publish(31)
    with pytest.raises(zlink.RequestError) as raised:
        entry.wait_request()
    assert raised.value.result == zlink.RequestResult.TIMED_OUT
    assert closer.closed == 1


def test_provisional_registry_exists_before_final_submission():
    socket = type("Socket", (), {"_handle": 1})()
    owner = CompletionOwner(socket)
    owner._start_runtime_owner_locked = lambda: None
    entry = _CompletionEntry(ZLINK_COMPLETION_SEND)
    owner._register(entry)
    assert owner._entries[entry.context] is entry
    entry.fail_submit()
    owner._unregister(entry)
    assert owner._entries == {}


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
