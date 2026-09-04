import asyncio
import errno
import uuid
from unittest.mock import patch

import pytest
import zlink


async def _next_event_loop_turn():
    loop = asyncio.get_running_loop()
    ready = loop.create_future()
    loop.call_soon(ready.set_result, None)
    await ready


def _serve(router, count):
    poller = zlink.create_poller()
    events = zlink.create_poll_events(1)
    received_payloads = []
    poller.add_socket(router, zlink.PollEventFlag.POLLIN, 1)
    try:
        for _ in range(count):
            assert poller.wait(events, 5000) == 1
            request = zlink.create_received()
            try:
                assert router.recv_into(request)
                payload = request.to_bytes_list()[0]
                received_payloads.append(payload)
                if request.reply_token is not None:
                    request.reply().message(b"reply:" + payload).submit()
            finally:
                request.close()
    finally:
        poller.close()
    return received_payloads


def _observe_owner(owner):
    native_submit = owner._submit_parts
    native_capture = owner._capture_writable
    submissions = []
    writables = []

    def observe_submit(target, native_parts, flags, entry=None, timeout_ms=None):
        result = native_submit(
            target,
            native_parts,
            flags,
            entry,
            timeout_ms,
        )
        submissions.append(
            {
                "result": result[0],
                "errno": result[1],
                "completion_id": result[2],
                "context": 0 if entry is None else entry.context,
                "target": None if target is None else bytes(target),
                "timeout_ms": timeout_ms,
            }
        )
        return result

    def observe_writable(entry, completion):
        size = int(completion.peer_rid.size)
        writables.append(
            {
                "completion_id": int(completion.completion_id),
                "context": int(completion.user_context or 0),
                "target": (
                    None
                    if size == 0
                    else bytes(completion.peer_rid.data[:size])
                ),
                "send_result": int(completion.send_result),
                "terminal_errno": int(completion.send_terminal_errno),
            }
        )
        return native_capture(entry, completion)

    return submissions, writables, observe_submit, observe_writable


def _configure(dealer, router):
    dealer.options.linger_ms = 0
    router.options.linger_ms = 0
    dealer.options.immediate = True
    dealer.options.send_high_water_mark = 512
    router.options.receive_high_water_mark = 512


@pytest.mark.parametrize("round_index", range(5))
def test_request_hwm_waits_for_its_writable_then_retries_and_receives_reply(
    round_index,
):
    async def exercise():
        context = zlink.create_context()
        context.options.auto_hwm_enabled = False
        dealer = zlink.create_dealer_socket(context)
        router = zlink.create_router_socket(context)
        tasks = []
        try:
            _configure(dealer, router)
            endpoint = f"inproc://python-request-hwm-{round_index}-{uuid.uuid4().hex}"
            router.bind(endpoint)
            dealer.connect(endpoint)

            dealer.send().message(b"attach-barrier").submit_sync()
            barrier = zlink.create_received()
            try:
                assert router.recv_into(barrier)
            finally:
                barrier.close()

            owner = dealer._completion_owner
            submissions, writables, observe_submit, observe_writable = (
                _observe_owner(owner)
            )
            sources = []
            expected = []
            with (
                patch.object(owner, "_submit_parts", side_effect=observe_submit),
                patch.object(
                    owner,
                    "_capture_writable",
                    side_effect=observe_writable,
                ),
            ):
                for index in range(512):
                    source = bytearray(index.to_bytes(4, "little") + b"x" * 60)
                    sources.append(source)
                    expected.append(bytes(source))
                    tasks.append(
                        asyncio.create_task(
                            dealer.request()
                            .message(source)
                            .timeout(5)
                            .submit()
                        )
                    )
                    await _next_event_loop_turn()
                    if submissions[-1]["result"] == int(
                        zlink.SubmitResult.BACKPRESSURED
                    ):
                        break
                else:
                    raise AssertionError("request HWM did not backpressure")

                blocked = submissions[-1]
                assert blocked["errno"] == errno.EAGAIN
                assert blocked["completion_id"] != 0
                assert blocked["context"] != 0
                assert blocked["timeout_ms"] == 5000
                sources[-1][:] = b"z" * len(sources[-1])

                received = await asyncio.to_thread(_serve, router, len(tasks))
                replies = await asyncio.gather(*tasks)

            assert received == expected
            assert [parts[0].to_bytes() for parts in replies] == [
                b"reply:" + payload for payload in expected
            ]
            for parts in replies:
                for part in parts:
                    part.close()

            assert len(writables) == 1
            writable = writables[0]
            assert writable["completion_id"] == blocked["completion_id"]
            assert writable["context"] == blocked["context"]
            assert writable["target"] is None
            assert writable["send_result"] == 0
            assert writable["terminal_errno"] == 0

            retry = submissions[-1]
            assert retry["result"] == int(zlink.SubmitResult.OK)
            assert retry["completion_id"] != 0
            assert retry["context"] == blocked["context"]
            assert retry["timeout_ms"] == 5000
        finally:
            for task in tasks:
                if not task.done():
                    task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            dealer.close()
            router.close()
            context.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("round_index", range(5))
def test_connect_before_bind_request_resumes_from_writable(round_index):
    async def exercise():
        context = zlink.create_context()
        dealer = zlink.create_dealer_socket(context)
        router = zlink.create_router_socket(context)
        task = None
        try:
            _configure(dealer, router)
            endpoint = (
                f"inproc://python-request-prebind-{round_index}-{uuid.uuid4().hex}"
            )
            dealer.connect(endpoint)
            owner = dealer._completion_owner
            submissions, writables, observe_submit, observe_writable = (
                _observe_owner(owner)
            )
            source = bytearray(b"prebind-request")
            expected = bytes(source)
            with (
                patch.object(owner, "_submit_parts", side_effect=observe_submit),
                patch.object(
                    owner,
                    "_capture_writable",
                    side_effect=observe_writable,
                ),
            ):
                task = asyncio.create_task(
                    dealer.request().message(source).timeout(5).submit()
                )
                await _next_event_loop_turn()
                blocked = submissions[0]
                assert blocked["result"] == int(zlink.SubmitResult.BACKPRESSURED)
                assert blocked["errno"] == errno.EAGAIN
                assert blocked["completion_id"] != 0
                source[:] = b"z" * len(source)

                router.bind(endpoint)
                received, reply = await asyncio.gather(
                    asyncio.to_thread(_serve, router, 1),
                    task,
                )

            assert received == [expected]
            assert [part.to_bytes() for part in reply] == [b"reply:" + expected]
            for part in reply:
                part.close()
            assert len(writables) == 1
            assert writables[0]["completion_id"] == blocked["completion_id"]
            assert submissions[-1]["result"] == int(zlink.SubmitResult.OK)
            assert submissions[-1]["context"] == blocked["context"]
        finally:
            if task is not None and not task.done():
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
            dealer.close()
            router.close()
            context.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("round_index", range(5))
def test_close_clears_request_wait_token_with_typed_failure(round_index):
    async def exercise():
        context = zlink.create_context()
        dealer = zlink.create_dealer_socket(context)
        task = None
        try:
            dealer.options.linger_ms = 0
            dealer.options.immediate = True
            endpoint = (
                f"inproc://python-request-close-{round_index}-{uuid.uuid4().hex}"
            )
            dealer.connect(endpoint)
            owner = dealer._completion_owner
            submissions, _, observe_submit, _ = _observe_owner(owner)
            with patch.object(
                owner, "_submit_parts", side_effect=observe_submit
            ):
                task = asyncio.create_task(
                    dealer.request().message(b"close-request").timeout(5).submit()
                )
                await _next_event_loop_turn()
                assert submissions[0]["result"] == int(
                    zlink.SubmitResult.BACKPRESSURED
                )
                assert submissions[0]["completion_id"] != 0
                assert owner._entries
                assert owner._entries_by_id
                dealer.close()
                with pytest.raises(zlink.SubmitError) as raised:
                    await task
                assert raised.value.result == zlink.SubmitResult.TERMINATED
                assert raised.value.native_errno == getattr(
                    errno, "ESHUTDOWN", errno.ECANCELED
                )
                assert not owner._entries
                assert not owner._entries_by_id
        finally:
            if task is not None and not task.done():
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
            dealer.close()
            context.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("round_index", range(5))
def test_send_and_request_wait_tokens_share_completion_owner(round_index):
    async def exercise():
        context = zlink.create_context()
        dealer = zlink.create_dealer_socket(context)
        router = zlink.create_router_socket(context)
        send_task = None
        request_task = None
        try:
            _configure(dealer, router)
            endpoint = (
                f"inproc://python-mixed-writable-{round_index}-{uuid.uuid4().hex}"
            )
            dealer.connect(endpoint)
            owner = dealer._completion_owner
            submissions, writables, observe_submit, observe_writable = (
                _observe_owner(owner)
            )
            with (
                patch.object(owner, "_submit_parts", side_effect=observe_submit),
                patch.object(
                    owner,
                    "_capture_writable",
                    side_effect=observe_writable,
                ),
            ):
                send_task = asyncio.create_task(
                    dealer.send().message(b"mixed-send").submit()
                )
                request_task = asyncio.create_task(
                    dealer.request().message(b"mixed-request").timeout(5).submit()
                )
                await _next_event_loop_turn()
                await _next_event_loop_turn()
                blocked = [
                    record
                    for record in submissions
                    if record["result"] == int(zlink.SubmitResult.BACKPRESSURED)
                ]
                assert len(blocked) == 2
                assert len({record["completion_id"] for record in blocked}) == 2

                router.bind(endpoint)
                received, _, reply = await asyncio.gather(
                    asyncio.to_thread(_serve, router, 2),
                    send_task,
                    request_task,
                )

            assert set(received) == {b"mixed-send", b"mixed-request"}
            assert [part.to_bytes() for part in reply] == [
                b"reply:mixed-request"
            ]
            for part in reply:
                part.close()
            assert len(writables) == 2
            assert {record["completion_id"] for record in writables} == {
                record["completion_id"] for record in blocked
            }
            request_retries = [
                record
                for record in submissions
                if record["timeout_ms"] == 5000
                and record["result"] == int(zlink.SubmitResult.OK)
            ]
            assert len(request_retries) == 1
            assert request_retries[0]["completion_id"] != 0
        finally:
            for task in (send_task, request_task):
                if task is not None and not task.done():
                    task.cancel()
            await asyncio.gather(
                *(task for task in (send_task, request_task) if task is not None),
                return_exceptions=True,
            )
            dealer.close()
            router.close()
            context.close()

    asyncio.run(exercise())
