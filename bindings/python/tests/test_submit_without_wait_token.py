# SPDX-License-Identifier: MPL-2.0

import asyncio
from contextlib import nullcontext
import errno

import pytest
import zlink
from zlink._runtime.handles.native_support import _native_extension


@pytest.mark.parametrize("operation", ["send", "request"])
@pytest.mark.parametrize("terminal", ["submit", "submit_sync"])
def test_tokenless_backpressure_preserves_core_result(monkeypatch, operation, terminal):
    async def exercise():
        source = b"caller owned"
        payload_context = (
            zlink.Message.from_(source)
            if _native_extension is not None
            else nullcontext(source)
        )
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as socket,
            zlink.create_poller() as poller,
            payload_context as payload,
        ):
            owner = socket._completion_owner
            if terminal == "submit":
                poller.add_socket(socket, zlink.PollEventFlag.POLLCOMPLETION, 1)

            calls = []
            results = []

            def reject(_target, parts, _flags, _entry=None, _timeout_ms=None):
                owner._close_unsubmitted(parts)
                calls.append(None)
                return int(zlink.SubmitResult.BACKPRESSURED), errno.EAGAIN, 0

            attempt_name = f"_attempt_{operation}"
            attempt = getattr(owner, attempt_name)

            def observe_attempt(entry):
                outcome = attempt(entry)
                results.append(getattr(outcome, "result", outcome))
                return outcome

            monkeypatch.setattr(owner, "_submit_parts", reject)
            monkeypatch.setattr(owner, attempt_name, observe_attempt)
            builder = getattr(socket, operation)().message(payload)
            if operation == "request":
                builder.timeout(1)

            with pytest.raises(zlink.SubmitError) as raised:
                getattr(builder, terminal)()

            assert raised.value.result is zlink.SubmitResult.BACKPRESSURED
            assert raised.value.internal_errno == errno.EAGAIN
            if isinstance(payload, zlink.Message):
                assert payload.to_bytes() == source
            assert len(calls) == 1
            assert not owner._entries
            assert not owner._entries_by_id
            if terminal == "submit":
                assert results == [zlink.SubmitResult.BACKPRESSURED]

    asyncio.run(exercise())
