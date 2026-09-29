"""Async submit runs outside the owner lock and joins early completions.

Core allows concurrent submits on one socket (core socket README §2), so the
binding keeps no lock around the Core call. A WRITABLE record that a drain
reads before the submit call returns is kept and joined when the token is
published (async-execution-model §5). A caller that abandons an operation
never has its staged message resubmitted (async-execution-model §6).
"""

import asyncio
import ctypes
import errno
import threading
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
from zlink._runtime.messaging import routed_async
from zlink._runtime.messaging.request_reply import _timeout_to_ms

from test_completion_projection_contract import completion_runtime  # noqa: F401

BACKPRESSURED = (int(zlink.SubmitResult.BACKPRESSURED), errno.EAGAIN)
OK = (int(zlink.SubmitResult.OK), 0)


def _writable(completion_id, context):
    completion = ZlinkCompletion()
    completion.struct_size = ctypes.sizeof(ZlinkCompletion)
    completion.kind = ZLINK_COMPLETION_WRITABLE
    completion.completion_id = completion_id
    completion.user_context = context
    completion.send_result = ZLINK_SEND_ADMITTED
    return completion


def _owner(runtime):
    owner = runtime.CompletionOwner(SimpleNamespace(_handle=1))
    public_owner = object()
    owner.transfer_to_public(public_owner)
    return owner, public_owner


def _submit(owner, operation):
    if operation == "send":
        return owner.submit_send(None, b"payload")
    return owner.submit_request(None, b"payload", 1000)


async def _turn():
    loop = asyncio.get_running_loop()
    ready = loop.create_future()
    loop.call_soon(ready.set_result, None)
    await ready


def _lock_is_free_for_another_thread(owner):
    acquired = []

    def probe():
        if owner._lock.acquire(timeout=2):
            acquired.append(True)
            owner._lock.release()

    thread = threading.Thread(target=probe)
    thread.start()
    thread.join()
    return bool(acquired)


@pytest.mark.parametrize("operation", ("send", "request"))
def test_submit_call_holds_no_owner_lock(completion_runtime, operation):  # noqa: F811
    owner, _ = _owner(completion_runtime)
    seen = []

    def submit(target, native_parts, flags, entry, timeout_ms=None):
        seen.append(_lock_is_free_for_another_thread(owner))
        owner._close_unsubmitted(native_parts)
        return OK[0], 0, 0 if operation == "send" else 41

    async def exercise():
        with patch.object(owner, "_submit_parts", side_effect=submit):
            _submit(owner, operation)

    asyncio.run(exercise())
    assert seen == [True]


@pytest.mark.parametrize("operation", ("send", "request"))
def test_writable_read_during_the_submit_call_is_joined(
    completion_runtime, operation  # noqa: F811
):
    owner, _ = _owner(completion_runtime)
    submissions = []

    def submit(target, native_parts, flags, entry, timeout_ms=None):
        assert flags == ZLINK_DONTWAIT
        submissions.append(entry)
        owner._close_unsubmitted(native_parts)
        if len(submissions) == 1:
            # A drain on another thread reads the WRITABLE record before the
            # submit call returns, i.e. before the token is published.
            assert entry.completion_id == 0
            assert owner._entries[entry.context] is entry
            assert not owner._capture_writable(entry, _writable(71, entry.context))
            return BACKPRESSURED[0], BACKPRESSURED[1], 71
        return OK[0], 0, 0 if operation == "send" else 72

    async def exercise():
        with patch.object(owner, "_submit_parts", side_effect=submit):
            submission = _submit(owner, operation)
            assert submission[0] == zlink.SubmitResult.BACKPRESSURED
            admitted = submission[1]
            await asyncio.wait_for(admitted, 5)
            return submission

    asyncio.run(exercise())
    assert len(submissions) == 2
    assert submissions[0] is submissions[1]
    assert not owner._early_writable
    assert submissions[0].payload is None


@pytest.mark.parametrize("operation", ("send", "request"))
def test_abandoned_operation_is_not_resubmitted(completion_runtime, operation):  # noqa: F811
    owner, public_owner = _owner(completion_runtime)
    submissions = []

    def submit(target, native_parts, flags, entry, timeout_ms=None):
        submissions.append(entry)
        owner._close_unsubmitted(native_parts)
        return BACKPRESSURED[0], BACKPRESSURED[1], 71

    receives = []

    def receive(handle, output, flags):
        receives.append(None)
        if len(receives) > 1:
            return int(zlink.RecvResult.NO_DATA)
        completion = _writable(71, submissions[0].context)
        ctypes.memmove(output, ctypes.byref(completion), ctypes.sizeof(completion))
        return int(zlink.RecvResult.OK)

    async def exercise():
        with (
            patch.object(owner, "_submit_parts", side_effect=submit),
            patch.object(lib(), "zlink_completion_recv", side_effect=receive),
        ):
            submission = _submit(owner, operation)
            entry = submissions[0]
            assert entry.payload is not None
            # The caller drops the awaitable before the WRITABLE arrives.
            submission[1].cancel()
            await _turn()
            assert owner.drain(public_owner).total_count == 1
            await _turn()
            return submission, entry

    submission, entry = asyncio.run(exercise())
    assert len(submissions) == 1
    assert entry.settled
    assert entry.payload is None
    assert not entry.waiting_native
    assert submission[1].cancelled()
    assert not owner._entries
    assert not owner._entries_by_id


def test_request_without_timeout_passes_zero_to_core():
    assert _timeout_to_ms(None) == 0
    assert _timeout_to_ms(0) == 0
    assert _timeout_to_ms(1.5) == 1500


def test_request_without_timeout_uses_the_socket_request_timeout_option():
    context = zlink.create_context()
    router = zlink.create_router_socket(context)
    dealer = zlink.create_dealer_socket(context)
    try:
        dealer.options.linger_ms = 0
        router.options.linger_ms = 0
        dealer.options.request_timeout_ms = 150
        router.bind("inproc://join-contract-timeout")
        dealer.connect("inproc://join-contract-timeout")
        # The router never replies; Core ends the request with the socket
        # option, the binding adds no timeout of its own.
        with pytest.raises(zlink.RequestError) as raised:
            dealer.request().message(b"ping").submit_sync()
        assert raised.value.result == zlink.RequestResult.TIMED_OUT
    finally:
        dealer.close()
        router.close()
        context.close()
