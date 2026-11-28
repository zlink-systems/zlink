# SPDX-License-Identifier: MPL-2.0

import asyncio
import ctypes
import errno
import threading
from collections import deque
from concurrent.futures import Future

import pytest
import zlink
from zlink._native.ffi import (
    ZLINK_COMPLETION_REQUEST,
    ZLINK_COMPLETION_WRITABLE,
    ZLINK_SEND_ADMITTED,
    ZlinkCompletion,
    lib,
)

from test_invalid_wait_token_errno import assert_protocol_failure


@pytest.mark.parametrize("operation", ["send", "request"])
@pytest.mark.parametrize(
    "kind", [ZLINK_COMPLETION_WRITABLE, ZLINK_COMPLETION_REQUEST],
    ids=["writable", "request-completion"],
)
@pytest.mark.parametrize("early", [True, False], ids=["during-submit", "after-submit"])
def test_late_invalid_wait_token_is_discarded(monkeypatch, operation, kind, early):
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as socket,
            zlink.create_poller() as poller,
        ):
            poller.add_socket(socket, zlink.PollEventFlag.POLLCOMPLETION, 1)
            owner = socket._completion_owner
            token = 71
            contexts = []
            records = deque()
            closed = []
            native = lib()
            native_close = native.zlink_completion_close

            def close(pointer):
                record = ctypes.cast(pointer, ctypes.POINTER(ZlinkCompletion)).contents
                closed.append((record.kind, record.user_context))
                native_close(pointer)

            def receive(_handle, output, _flags):
                if not records:
                    return int(zlink.RecvResult.NO_DATA)
                record = records.popleft()
                ctypes.memmove(output, ctypes.byref(record), ctypes.sizeof(record))
                return int(zlink.RecvResult.OK)

            def enqueue(record_kind, user_context):
                records.append(ZlinkCompletion(
                    struct_size=ctypes.sizeof(ZlinkCompletion), kind=record_kind,
                    completion_id=token, user_context=user_context,
                    send_result=ZLINK_SEND_ADMITTED,
                    request_result=int(zlink.RequestResult.TIMED_OUT),
                ))

            def drain_one():
                drained = Future()

                def drain():
                    try:
                        drained.set_result(owner.drain(poller).total_count)
                    except BaseException as error:
                        drained.set_exception(error)

                threading.Thread(target=drain, daemon=True).start()
                assert drained.result(timeout=2) == 1

            def drain_late_record():
                enqueue(kind, contexts[0])
                drain_one()
                assert closed == [(kind, contexts[0])]
                assert not owner._early_writable

            def submit(_target, parts, _flags, entry, _timeout_ms=None):
                owner._close_unsubmitted(parts)
                contexts.append(entry.context)
                if len(contexts) == 1:
                    return int(zlink.SubmitResult.BACKPRESSURED), errno.EINVAL, token
                if len(contexts) == 2:
                    if early:
                        drain_late_record()
                        enqueue(ZLINK_COMPLETION_WRITABLE, contexts[1])
                        drain_one()
                    return int(zlink.SubmitResult.BACKPRESSURED), errno.EAGAIN, token
                return int(zlink.SubmitResult.OK), 0, 0

            monkeypatch.setattr(owner, "_submit_parts", submit)
            monkeypatch.setattr(native, "zlink_completion_recv", receive)
            monkeypatch.setattr(native, "zlink_completion_close", close)

            rejected = getattr(socket, operation)().message(b"rejected")
            if operation == "request":
                rejected.timeout(1)
            with pytest.raises(zlink.SubmitError) as failure:
                rejected.submit()
            assert_protocol_failure(failure.value)
            del failure

            submission = socket.send().message(b"next").submit()
            assert submission.result == zlink.SubmitResult.BACKPRESSURED
            assert not submission.admitted.done()
            if not early:
                drain_late_record()
                assert not submission.admitted.done()
                enqueue(ZLINK_COMPLETION_WRITABLE, contexts[1])
                drain_one()
            assert len(contexts) == 2

            await asyncio.wait_for(submission.admitted, timeout=2)
            assert contexts == [contexts[0], contexts[1], contexts[1]]
            assert 0 < contexts[0] < contexts[1]
            assert closed == [(kind, contexts[0]), (ZLINK_COMPLETION_WRITABLE, contexts[1])]
            assert not owner._entries
            assert not owner._entries_by_id
            assert not owner._early_writable

    asyncio.run(exercise())
