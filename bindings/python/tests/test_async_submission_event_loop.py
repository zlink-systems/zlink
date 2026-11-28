# SPDX-License-Identifier: MPL-2.0

import asyncio

import zlink


def test_submission_futures_belong_to_the_loop_that_submits():
    async def exercise():
        loop = asyncio.get_running_loop()
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as dealer,
            zlink.create_router_socket(context) as router,
            zlink.create_poller() as poller,
            zlink.create_received() as received,
        ):
            dealer.options.linger_ms = router.options.linger_ms = 0
            router.options.receive_timeout_ms = 1000
            router.bind("inproc://submission-event-loop")
            dealer.connect("inproc://submission-event-loop")
            poller.add_socket(dealer, zlink.PollEventFlag.POLLCOMPLETION, 1)

            sent = dealer.send().message(b"send").submit()
            assert sent.admitted.get_loop() is loop
            await sent.admitted
            assert router.recv_into(received)

            request = dealer.request().message(b"request").timeout(2).submit()
            assert request.admitted.get_loop() is loop
            assert request.reply.get_loop() is loop
            await request.admitted
            assert router.recv_into(received)
            received.reply().message(b"reply").submit()
            assert await asyncio.to_thread(poller.wait, zlink.create_poll_events(1), 1000) == 1
            parts = await request.reply
            try:
                assert [part.to_bytes() for part in parts] == [b"reply"]
            finally:
                for part in parts:
                    part.close()

    asyncio.run(exercise())
