# SPDX-License-Identifier: MPL-2.0

import asyncio
import uuid
from unittest.mock import patch

import zlink


def test_first_request_attempt_and_retry_use_the_pre_submit_snapshot():
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as dealer,
            zlink.create_router_socket(context) as router,
            zlink.create_poller() as poller,
        ):
            dealer.options.linger_ms = router.options.linger_ms = 0
            dealer.options.immediate = True
            router.options.receive_timeout_ms = 5000
            endpoint = f"inproc://request-staging-{uuid.uuid4().hex}"
            dealer.connect(endpoint)
            poller.add_socket(dealer, zlink.PollEventFlag.POLLCOMPLETION, 1)

            source = bytearray(b"request-before-first-call")
            expected = bytes(source)
            owner = dealer._completion_owner
            native_submit = owner._submit_parts

            def submit(target, parts, flags, entry, timeout_ms):
                result = native_submit(target, parts, flags, entry, timeout_ms)
                if result[0] == zlink.SubmitResult.BACKPRESSURED:
                    # The binding has not processed the native result yet.
                    source[:] = b"x" * len(source)
                return result

            with patch.object(owner, "_submit_parts", side_effect=submit):
                submission = dealer.request().message(source).timeout(5).submit()
                assert submission.result == zlink.SubmitResult.BACKPRESSURED
                assert not submission.admitted.done()
                router.bind(endpoint)

                events = zlink.create_poll_events(1)
                while not submission.admitted.done():
                    assert poller.wait(events, 5000) == 1
                    await asyncio.sleep(0)
                await submission.admitted
                with zlink.create_received() as received:
                    assert router.recv_into(received)
                    actual = received.to_bytes_list()
                    received.reply().message(b"reply").submit()
                while not submission.reply.done():
                    assert poller.wait(events, 5000) == 1
                for part in await submission.reply:
                    part.close()
                assert actual == [expected]

    asyncio.run(exercise())
