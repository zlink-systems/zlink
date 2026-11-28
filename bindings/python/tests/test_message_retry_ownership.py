# SPDX-License-Identifier: MPL-2.0

import asyncio
import errno
import uuid

import pytest
import zlink


def _receive(router, payload, *, reply=False):
    with zlink.create_received() as received:
        assert router.recv_into(received)
        assert received.to_bytes_list() == [payload]
        if reply:
            received.reply().message(b"reply:" + payload).submit()


@pytest.mark.parametrize("operation", ("send", "request"))
@pytest.mark.parametrize("outcome", ("admit", "cancel", "close"))
def test_hwm_wait_consumes_original_message(operation, outcome):
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as dealer,
            zlink.create_router_socket(context) as router,
            zlink.create_poller() as poller,
        ):
            context.options.auto_hwm_enabled = False
            dealer.options.linger_ms = router.options.linger_ms = 0
            dealer.options.immediate = True
            dealer.options.send_high_water_mark = 512
            router.options.receive_high_water_mark = 512
            router.options.receive_timeout_ms = 5000
            endpoint = f"inproc://message-retry-ownership-{uuid.uuid4().hex}"
            router.bind(endpoint)
            dealer.connect(endpoint)
            dealer.send().message(b"attach").submit_sync()
            _receive(router, b"attach")

            events = zlink.create_poll_events(1)
            poller.add_socket(dealer, zlink.PollEventFlag.POLLCOMPLETION, 1)
            accepted = []
            replies = []
            is_request = operation == "request"
            try:
                for index in range(512):
                    payload = index.to_bytes(4, "little") + b"x" * 60
                    with zlink.Message.from_(payload) as source:
                        builder = getattr(dealer, operation)().message(source)
                        if is_request:
                            builder.timeout(5)
                        submission = builder.submit()
                        size_after_submit = source.size()
                        if is_request:
                            replies.append(submission.reply)
                        if submission.result == zlink.SubmitResult.OK:
                            assert size_after_submit == 0
                            accepted.append(payload)
                            continue
                        assert submission.result == zlink.SubmitResult.BACKPRESSURED
                        assert accepted
                        assert not submission.admitted.done()

                        if outcome == "close":
                            poller.remove_socket(dealer)
                            dealer.close()
                            stages = [submission.admitted]
                            if is_request:
                                stages.append(submission.reply)
                            for stage in stages:
                                with pytest.raises(zlink.SubmitError) as raised:
                                    await stage
                                assert raised.value.result == zlink.SubmitResult.TERMINATED
                        else:
                            if outcome == "cancel":
                                waiter = (
                                    submission.reply if is_request else submission.admitted
                                )
                                assert waiter.cancel()
                            for expected in accepted:
                                _receive(
                                    router, expected, reply=is_request and outcome == "admit"
                                )
                            if outcome == "cancel":
                                assert poller.wait(events, 5000) == 1
                            while not submission.admitted.done():
                                assert poller.wait(events, 5000) == 1
                                await asyncio.sleep(0)
                            if outcome == "cancel":
                                with pytest.raises(asyncio.CancelledError):
                                    await submission.admitted
                                with zlink.create_received() as received:
                                    assert not router.recv_into(
                                        received, flags=zlink.RecvFlags.DONT_WAIT
                                    )
                            else:
                                await submission.admitted
                                _receive(router, payload, reply=is_request)
                                if is_request:
                                    while not submission.reply.done():
                                        assert poller.wait(events, 5000) == 1
                                    assert [part.to_bytes() for part in await submission.reply] == [
                                        b"reply:" + payload
                                    ]

                        assert (size_after_submit, source.size()) == (0, 0)
                        break
                else:
                    pytest.fail(f"{operation} HWM did not backpressure")
            finally:
                for reply in replies:
                    if not reply.done():
                        reply.cancel()
                    elif not reply.cancelled() and reply.exception() is None:
                        for part in reply.result():
                            part.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("operation", ("send", "request"))
def test_tokenless_backpressure_preserves_original_message(operation, monkeypatch):
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as dealer,
            zlink.create_poller() as poller,
            zlink.Message.from_(b"caller-owned") as source,
        ):
            poller.add_socket(dealer, zlink.PollEventFlag.POLLCOMPLETION, 1)
            owner = dealer._completion_owner

            def refuse(target, parts, flags, entry=None, timeout_ms=None):
                owner._close_unsubmitted(parts)
                return int(zlink.SubmitResult.BACKPRESSURED), errno.EAGAIN, 0

            monkeypatch.setattr(owner, "_submit_parts", refuse)
            with pytest.raises(zlink.SubmitError) as raised:
                getattr(dealer, operation)().message(source).submit()
            assert raised.value.result == zlink.SubmitResult.BACKPRESSURED
            assert raised.value.internal_errno == errno.EAGAIN
            assert source.to_bytes() == b"caller-owned"

    asyncio.run(exercise())
