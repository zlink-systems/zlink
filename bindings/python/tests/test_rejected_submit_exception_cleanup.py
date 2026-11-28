# SPDX-License-Identifier: MPL-2.0

import asyncio
import errno

import pytest
import zlink


@pytest.mark.parametrize("operation", ["send", "request"])
@pytest.mark.parametrize("failure_site", ["projection", "publication"])
def test_rejected_submit_exception_releases_registration(monkeypatch, operation, failure_site):
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as socket,
            zlink.create_poller() as poller,
        ):
            poller.add_socket(socket, zlink.PollEventFlag.POLLCOMPLETION, 1)
            owner = socket._completion_owner
            failure = RuntimeError("injected submit projection failure")
            entries = []
            completion_id = 42 if failure_site == "publication" else 0

            def reject(_target, parts, _flags, entry, _timeout_ms=None):
                owner._close_unsubmitted(parts)
                entries.append(entry)
                return int(zlink.SubmitResult.BACKPRESSURED), errno.EAGAIN, completion_id

            if failure_site == "projection":
                def fail_projection(*args):
                    raise failure

                monkeypatch.setattr(owner, "_finish_rejected_submit_locked", fail_projection)
            else:
                publish = owner._publish_writable_locked

                def fail_after_publication(entry, token):
                    publish(entry, token)
                    assert owner._entries_by_id[token] is entry
                    raise failure

                monkeypatch.setattr(owner, "_publish_writable_locked", fail_after_publication)

            monkeypatch.setattr(owner, "_submit_parts", reject)
            builder = getattr(socket, operation)().message(b"payload")
            if operation == "request":
                builder.timeout(1)
            with pytest.raises(RuntimeError) as raised:
                builder.submit()

            assert raised.value is failure
            entry, = entries
            assert entry.context not in owner._entries
            assert entry.context not in owner._early_writable
            assert completion_id not in owner._entries_by_id
            assert entry.payload is None

    asyncio.run(exercise())
