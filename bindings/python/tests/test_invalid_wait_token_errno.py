# SPDX-License-Identifier: MPL-2.0

import asyncio
import errno

import pytest
import zlink
from zlink._runtime.messaging.routed_async import (
    ZLINK_COMPLETION_WRITABLE,
    ZLINK_SEND_ADMITTED,
    ZlinkCompletion,
)


@pytest.mark.parametrize("operation", ["send", "request"])
@pytest.mark.parametrize("retry", [False, True])
def test_invalid_wait_token_errno_cleans_up_submission(monkeypatch, operation, retry):
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as socket,
            zlink.create_poller() as poller,
            zlink.Message.from_(b"caller payload") as message,
        ):
            poller.add_socket(socket, zlink.PollEventFlag.POLLCOMPLETION, 1)
            owner = socket._completion_owner
            attempts = []

            def reject(_target, parts, _flags, entry, _timeout_ms=None):
                owner._close_unsubmitted(parts)
                attempts.append(entry)
                if retry and len(attempts) == 1:
                    return int(zlink.SubmitResult.BACKPRESSURED), errno.EAGAIN, 71
                return int(zlink.SubmitResult.BACKPRESSURED), errno.EINVAL, 72

            monkeypatch.setattr(owner, "_submit_parts", reject)
            builder = getattr(socket, operation)().message(message)
            if operation == "request":
                builder.timeout(1)

            if retry:
                submission = builder.submit()
                assert submission.result == zlink.SubmitResult.BACKPRESSURED
                assert message.to_bytes() == b""
                entry, = attempts
                assert entry.payload is not None
                completion = ZlinkCompletion(
                    kind=ZLINK_COMPLETION_WRITABLE, completion_id=71,
                    user_context=entry.context, send_result=ZLINK_SEND_ADMITTED,
                )
                retries = []
                owner._process_completion(completion, retries)
                assert retries == [entry]
                owner._dispatch_retry(entry)
                stages = [submission.admitted]
                if operation == "request":
                    stages.append(submission.reply)
                assert all(stage.done() for stage in stages)
                for error in await asyncio.gather(*stages, return_exceptions=True):
                    assert isinstance(error, zlink.SubmitError)
                    assert_protocol_failure(error)
                assert message.to_bytes() == b""
                assert attempts == [entry, entry]
            else:
                with pytest.raises(zlink.SubmitError) as raised:
                    builder.submit()
                assert_protocol_failure(raised.value)
                assert message.to_bytes() == b"caller payload"
                entry, = attempts

            assert entry.payload is None
            assert not owner._entries
            assert not owner._entries_by_id
            assert not owner._early_writable

    asyncio.run(exercise())


def assert_protocol_failure(error):
    assert (error.result, error.internal_errno) == (
        zlink.SubmitResult.INTERNAL_ERROR, errno.EPROTO,
    )
