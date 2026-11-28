# SPDX-License-Identifier: MPL-2.0

import asyncio

import pytest
import zlink


def test_received_reply_after_socket_close_is_terminated():
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as dealer,
            zlink.create_router_socket(context) as router,
            zlink.create_poller() as poller,
            zlink.create_received() as request,
        ):
            dealer.options.linger_ms = router.options.linger_ms = 0
            router.options.receive_timeout_ms = 1000
            router.bind("inproc://reply-after-close")
            dealer.connect("inproc://reply-after-close")
            poller.add_socket(dealer, zlink.PollEventFlag.POLLCOMPLETION, 1)
            submission = dealer.request().message(b"request").timeout(2).submit()
            await submission.admitted
            assert router.recv_into(request)
            router.close()

            submission.reply.cancel()
            with pytest.raises(zlink.SubmitError) as raised:
                request.reply().message(b"reply").submit()
            assert raised.value.result is zlink.SubmitResult.TERMINATED

    asyncio.run(exercise())
