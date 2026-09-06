import asyncio
import ctypes
import errno
import importlib.util
from types import SimpleNamespace
from unittest.mock import patch

import pytest
import zlink
from zlink._native.ffi import (
    ZLINK_COMPLETION_WRITABLE,
    ZLINK_DONTWAIT,
    ZLINK_SEND_ADMITTED,
    ZlinkCompletion,
    lib,
)
from zlink._runtime.handles import native_support
from zlink._runtime.messaging import routed_async


@pytest.fixture(params=("native", "python"))
def completion_runtime(request):
    if request.param == "native":
        pytest.importorskip("zlink._native._zlink_native")
        assert not hasattr(routed_async.CompletionOwner._drain, "__code__")
        return routed_async

    # Load the fallback before the extension can replace its class methods.
    spec = importlib.util.spec_from_file_location(
        "zlink._runtime.messaging._completion_projection_fallback",
        routed_async.__file__,
    )
    runtime = importlib.util.module_from_spec(spec)
    with patch.object(native_support, "_native_extension", None):
        spec.loader.exec_module(runtime)
    assert runtime._native_extension is None
    assert hasattr(runtime.CompletionOwner._drain, "__code__")
    return runtime


@pytest.mark.parametrize("operation", ("send", "request"))
@pytest.mark.parametrize("lookup", ("token", "context"))
def test_writable_delivers_to_registered_waiter_without_reading_rid_echo(
    completion_runtime, operation, lookup
):
    runtime = completion_runtime
    owner = runtime.CompletionOwner(SimpleNamespace(_handle=1))
    target = b"submitted-route"
    if operation == "send":
        entry = runtime._SendEntry(None, target, [])
    else:
        entry = runtime._RequestEntry(None, 1000)
        entry.target = target
        entry.payload = []
    other = runtime._SendEntry(None, target, [])
    owner._register(entry)
    owner._register(other)
    assert entry.await_writable(71)
    assert other.await_writable(72)
    owner._track_native_wait_locked(other)
    if lookup == "token":
        owner._track_native_wait_locked(entry)

    completion = ZlinkCompletion()
    completion.struct_size = ctypes.sizeof(ZlinkCompletion)
    completion.kind = ZLINK_COMPLETION_WRITABLE
    completion.completion_id = 71
    completion.user_context = entry.context
    completion.peer_rid.size = len(target)
    completion.peer_rid.data[: len(target)] = target
    completion.send_result = ZLINK_SEND_ADMITTED
    receives = []

    def receive(handle, output, flags):
        assert handle == 1
        assert flags == ZLINK_DONTWAIT
        receives.append(None)
        if len(receives) == 1:
            ctypes.memmove(output, ctypes.byref(completion), ctypes.sizeof(completion))
            return int(zlink.RecvResult.OK)
        return int(zlink.RecvResult.NO_DATA)

    def reject_rid_read(_completion):
        raise AssertionError("The submit RID echo is owned by Core")

    native = lib()
    with (
        patch.object(native, "zlink_completion_recv", side_effect=receive),
        patch.object(
            native, "zlink_completion_close", wraps=native.zlink_completion_close
        ) as close,
        patch.object(ZlinkCompletion, "peer_rid", property(reject_rid_read)),
        patch.object(owner, "_dispatch_retry") as retry,
    ):
        result = owner.drain()

    assert result.total_count == 1
    assert result.request_count == 0
    assert len(receives) == 2
    close.assert_called_once()
    retry.assert_called_once_with(entry)
    assert entry.completion_id == 0
    assert not entry.waiting_native
    assert not entry.settled
    assert entry._error is None
    assert entry.target == target
    assert other.completion_id == 72
    assert other.waiting_native
    assert not other.settled
    assert owner._entries_by_id == {72: other}


@pytest.mark.parametrize("stage", ("initial", "retry"))
def test_request_tokenless_backpressure_preserves_core_error(
    completion_runtime, stage
):
    runtime = completion_runtime

    async def exercise():
        context = zlink.create_context()
        dealer = zlink.create_dealer_socket(context)
        dealer.options.linger_ms = 0
        owner = runtime.CompletionOwner(dealer)
        dealer._completion_owner = owner
        submissions = []
        task = None

        def submit(target, native_parts, flags, entry, timeout_ms):
            assert target is None
            assert flags == ZLINK_DONTWAIT
            assert timeout_ms == 1000
            submissions.append(entry)
            # The fake Core consumes each attempted part, including on refusal.
            owner._close_unsubmitted(native_parts)
            token = 71 if stage == "retry" and len(submissions) == 1 else 0
            return int(zlink.SubmitResult.BACKPRESSURED), errno.EAGAIN, token

        receives = []

        def receive(handle, output, flags):
            assert handle == dealer._handle
            assert flags == ZLINK_DONTWAIT
            receives.append(None)
            if len(receives) > 1:
                return int(zlink.RecvResult.NO_DATA)
            completion = ZlinkCompletion()
            completion.struct_size = ctypes.sizeof(ZlinkCompletion)
            completion.kind = ZLINK_COMPLETION_WRITABLE
            completion.completion_id = 71
            completion.user_context = submissions[0].context
            completion.send_result = ZLINK_SEND_ADMITTED
            ctypes.memmove(output, ctypes.byref(completion), ctypes.sizeof(completion))
            return int(zlink.RecvResult.OK)

        try:
            with (
                patch.object(owner, "_submit_parts", side_effect=submit),
                patch.object(owner, "_schedule_runtime_owner_locked") as schedule,
                patch.object(lib(), "zlink_completion_recv", side_effect=receive),
            ):
                operation = dealer.request().message(b"request").timeout(1)
                if stage == "retry":
                    task = asyncio.create_task(operation.submit())
                    turn = asyncio.get_running_loop().create_future()
                    asyncio.get_running_loop().call_soon(turn.set_result, None)
                    await turn
                    assert not task.done()
                    assert submissions[0].waiting_native
                    assert owner._entries_by_id == {71: submissions[0]}
                    schedule.assert_called_once()
                    schedule.reset_mock()
                    assert owner.drain().total_count == 1
                    result = task
                else:
                    result = operation.submit()

                with pytest.raises(zlink.SubmitError) as raised:
                    await result

                assert raised.value.result == zlink.SubmitResult.BACKPRESSURED
                assert raised.value.native_errno == errno.EAGAIN
                schedule.assert_not_called()
                assert len(submissions) == (2 if stage == "retry" else 1)
                assert len(receives) == (2 if stage == "retry" else 0)
                assert len({id(entry) for entry in submissions}) == 1
                entry = submissions[0]
                assert entry.settled
                assert not entry.waiting_native
                assert entry.completion_id == 0
                assert entry.payload is None
                assert not owner._entries
                assert not owner._entries_by_id
        finally:
            if task is not None and not task.done():
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
            dealer.close()
            context.close()

    asyncio.run(exercise())
