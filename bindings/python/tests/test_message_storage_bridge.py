"""The native storage boundary preserves copy and lifetime semantics."""

import asyncio
from array import array

import pytest
import zlink
from zlink._runtime.handles import native_support
from zlink._runtime.messaging.routed_async import _SendEntry


@pytest.mark.parametrize("use_extension", [True, False])
@pytest.mark.parametrize("source", [
    b"", b"a\x00b", bytearray(b"mutable"),
    memoryview(b"readonly"), memoryview(b"strided")[::2],
    array("I", [1, 2, 3]), b"large" * 20000,
])
def test_message_storage_copy_and_clone_lifetimes(monkeypatch, use_extension, source):
    if not use_extension:
        monkeypatch.setattr(native_support, "_native_extension", None)
    expected = bytes(source)
    original = zlink.Message.from_(source)
    clone = native_support._clone_native_msg(original._msg)
    try:
        assert original.to_bytes() == expected
        original.close()
        assert native_support._msg_to_bytes(clone) == expected
    finally:
        original.close()
        native_support.lib().zlink_msg_close(native_support.ctypes.byref(clone))


def test_copied_mutable_input_is_independent():
    source = bytearray(b"original")
    with zlink.Message.from_(source) as message:
        source[:] = b"modified"
        assert message.to_bytes() == b"original"


def test_send_only_allocates_future_when_waiting():
    async def exercise():
        loop = asyncio.get_running_loop()
        accepted = _SendEntry(loop, None, [])
        accepted.succeed_send()
        assert await accepted.wait_async() is None
        assert accepted.future is None

        rejected = _SendEntry(loop, None, [])
        failure = zlink.SubmitError(zlink.SubmitResult.NOT_CONNECTED, 1)
        rejected.fail(failure)
        with pytest.raises(zlink.SubmitError) as raised:
            await rejected.wait_async()
        assert raised.value is failure
        assert rejected.future is None

        pending = _SendEntry(loop, None, [])
        loop.call_soon(pending.succeed_send)
        assert await pending.wait_async() is None
        assert pending.future.done()

    asyncio.run(exercise())


def test_admitted_send_has_no_completion_registration_or_private_condition():
    from unittest.mock import patch

    async def exercise():
        with zlink.create_context() as context:
            with zlink.create_pair_socket(context) as sender, zlink.create_pair_socket(context) as receiver:
                sender.bind("inproc://storage-bridge-admitted")
                receiver.connect("inproc://storage-bridge-admitted")
                # Public blocking traffic provides the connection barrier.
                receiver.send().message(b"ready").submit_sync()
                with zlink.create_received() as received:
                    assert sender.recv_into(received)
                owner = sender._completion_owner
                attempt = owner._submit_parts
                observed = []

                def submit(*args, **kwargs):
                    entry = args[3]
                    assert not owner._entries
                    assert entry.condition is owner._state_changed
                    result = attempt(*args, **kwargs)
                    observed.append((entry, result))
                    return result

                with patch.object(owner, "_submit_parts", side_effect=submit):
                    await sender.send().message(b"payload").submit()
                assert len(observed) == 1
                entry, result = observed[0]
                assert result == (int(zlink.SubmitResult.OK), 0, 0)
                assert entry.future is None
                assert not owner._entries
                assert not owner._entries_by_id
                assert owner._runtime_poller is None
                with zlink.create_received() as received:
                    assert receiver.recv_into(received)
                    assert received.single_part_or_throw().to_bytes() == b"payload"

    asyncio.run(exercise())


def test_writable_received_during_submit_is_not_lost_before_registration():
    import ctypes
    import errno
    import threading
    from types import SimpleNamespace
    from unittest.mock import patch
    from zlink._native.ffi import (
        ZlinkCompletion, ZLINK_COMPLETION_WRITABLE, ZLINK_SEND_ADMITTED,
    )
    from zlink._runtime.messaging.routed_async import CompletionOwner

    async def exercise():
        owner = CompletionOwner(SimpleNamespace(_handle=1))
        public_owner = object()
        owner.transfer_to_public(public_owner)
        receiving = threading.Event()
        admitted = threading.Event()
        received = threading.Event()
        native = native_support.lib()
        submissions = []
        closed = []
        drained = []

        class CompletionNative:
            def __getattr__(self, name):
                return getattr(native, name)

            def zlink_completion_recv(self, handle, output, flags):
                if receiving.is_set():
                    return int(zlink.RecvResult.NO_DATA)
                receiving.set()
                assert admitted.wait(3)
                completion = ctypes.cast(output, ctypes.POINTER(ZlinkCompletion)).contents
                completion.kind = ZLINK_COMPLETION_WRITABLE
                completion.completion_id = 7
                completion.user_context = submissions[0].context
                completion.send_result = ZLINK_SEND_ADMITTED
                received.set()
                return int(zlink.RecvResult.OK)

            def zlink_completion_close(self, output):
                closed.append(1)
                return 0

        def submit(target, parts, flags, entry=None, timeout_ms=None):
            for part in parts:
                native.zlink_msg_close(ctypes.byref(part))
            submissions.append(entry)
            if len(submissions) == 1:
                admitted.set()
                assert received.wait(3)
                return int(zlink.SubmitResult.BACKPRESSURED), errno.EAGAIN, 7
            return int(zlink.SubmitResult.OK), 0, 0

        def drain():
            drained.append(owner.drain(public_owner).total_count)

        with patch("zlink._runtime.messaging.routed_async.lib", return_value=CompletionNative()), patch.object(owner, "_submit_parts", side_effect=submit):
            thread = threading.Thread(target=drain)
            thread.start()
            assert receiving.wait(3)
            try:
                await asyncio.wait_for(owner.submit_send(None, b"packet"), 3)
                thread.join(3)
                assert not thread.is_alive()
                assert drained == [1]
                assert closed == [1]
                assert len(submissions) == 2
                assert submissions[0] is submissions[1]
                assert not owner._entries
                assert not owner._entries_by_id
            finally:
                admitted.set()
                thread.join(3)
                owner.shutdown()
                owner.finish_shutdown()

    asyncio.run(exercise())
