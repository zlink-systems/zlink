import asyncio
import uuid

import pytest
import zlink


HWM_BYTES = 512
MAX_FILL_RECORDS = 4096
PAYLOAD_BYTES = 64


def _payload(sequence):
    return sequence.to_bytes(4, "little") + b"z" * (PAYLOAD_BYTES - 4)


def _configure_small_hwm(socket):
    socket.options.linger_ms = 0
    socket.options.send_high_water_mark = HWM_BYTES
    socket.options.receive_high_water_mark = HWM_BYTES


def _connect_ready(server, client, label):
    endpoint = f"inproc://python-{label}-{uuid.uuid4().hex}"
    server.bind(endpoint)
    client.connect(endpoint)
    client.send().message(b"ready").submit_sync()
    received = zlink.create_received()
    try:
        assert server.recv_into(received)
        assert received.to_bytes_list() == [b"ready"]
    finally:
        received.close()


def _receive(server):
    received = zlink.create_received()
    assert server.recv_into(received)
    return received


def _poll_until(poller, predicate):
    events = zlink.create_poll_events(4)
    for _ in range(32):
        if predicate():
            return
        assert poller.wait(events, 5000) > 0
    assert predicate()


@pytest.mark.parametrize("round_index", range(5))
def test_immediate_admission_returns_ok_with_completed_stage_before_reply(
    round_index,
):
    async def exercise():
        with zlink.create_context() as context:
            with zlink.create_router_socket(context) as server:
                with zlink.create_dealer_socket(context) as client:
                    _connect_ready(server, client, f"submit-ok-{round_index}")

                    send = client.send().message(b"send-ok").submit()
                    assert isinstance(send, zlink.SendSubmission)
                    assert send.result == zlink.SubmitResult.OK
                    assert send.admitted.done()
                    assert await send.admitted is None
                    received = _receive(server)
                    try:
                        assert received.to_bytes_list() == [b"send-ok"]
                    finally:
                        received.close()

                    request = (
                        client.request().message(b"request-ok").timeout(30).submit()
                    )
                    assert isinstance(request, zlink.RequestSubmission)
                    assert request.result == zlink.SubmitResult.OK
                    assert request.admitted.done()
                    assert await request.admitted is None
                    assert not request.reply.done()

                    received = _receive(server)
                    try:
                        assert received.to_bytes_list() == [b"request-ok"]
                        received.reply().message(b"reply-ok").submit()
                    finally:
                        received.close()
                    reply = await asyncio.wait_for(
                        asyncio.shield(request.reply), 5
                    )
                    try:
                        assert [part.to_bytes() for part in reply] == [b"reply-ok"]
                    finally:
                        for part in reply:
                            part.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("round_index", range(5))
def test_hwm_backpressure_admits_after_public_writable_progress_before_reply(
    round_index,
):
    async def exercise():
        with zlink.create_context() as context:
            context.options.auto_hwm_enabled = False
            with zlink.create_router_socket(context) as server:
                with zlink.create_dealer_socket(context) as client:
                    _configure_small_hwm(server)
                    _configure_small_hwm(client)
                    _connect_ready(server, client, f"submit-hwm-{round_index}")

                    with zlink.create_poller() as poller:
                        poller.add_socket(
                            client,
                            zlink.PollEventFlag.POLLOUT
                            | zlink.PollEventFlag.POLLCOMPLETION,
                            1,
                        )

                        admitted_sends = 0
                        waiting_send = None
                        for sequence in range(MAX_FILL_RECORDS):
                            submission = (
                                client.send().message(_payload(sequence)).submit()
                            )
                            if submission.result == zlink.SubmitResult.BACKPRESSURED:
                                waiting_send = submission
                                break
                            assert submission.result == zlink.SubmitResult.OK
                            assert submission.admitted.done()
                            admitted_sends += 1
                        assert admitted_sends > 0
                        assert waiting_send is not None
                        assert not waiting_send.admitted.done()

                        for sequence in range(admitted_sends):
                            received = _receive(server)
                            try:
                                assert received.to_bytes_list() == [_payload(sequence)]
                            finally:
                                received.close()
                        _poll_until(poller, waiting_send.admitted.done)
                        assert await waiting_send.admitted is None
                        assert waiting_send.result == zlink.SubmitResult.BACKPRESSURED
                        received = _receive(server)
                        try:
                            assert received.to_bytes_list() == [
                                _payload(admitted_sends)
                            ]
                        finally:
                            received.close()

                        admitted_requests = []
                        waiting_request = None
                        for sequence in range(MAX_FILL_RECORDS):
                            submission = (
                                client.request()
                                .message(_payload(sequence))
                                .timeout(30)
                                .submit()
                            )
                            if submission.result == zlink.SubmitResult.BACKPRESSURED:
                                waiting_request = submission
                                break
                            assert submission.result == zlink.SubmitResult.OK
                            assert submission.admitted.done()
                            admitted_requests.append(submission)
                        assert admitted_requests
                        assert waiting_request is not None
                        assert not waiting_request.admitted.done()
                        assert not waiting_request.reply.done()

                        for sequence in range(len(admitted_requests)):
                            received = _receive(server)
                            try:
                                assert received.to_bytes_list() == [
                                    _payload(sequence)
                                ]
                                received.reply().message(
                                    b"reply-" + sequence.to_bytes(4, "little")
                                ).submit()
                            finally:
                                received.close()
                        _poll_until(poller, waiting_request.admitted.done)
                        assert await waiting_request.admitted is None
                        assert (
                            waiting_request.result
                            == zlink.SubmitResult.BACKPRESSURED
                        )
                        assert not waiting_request.reply.done()

                        received = _receive(server)
                        try:
                            assert received.to_bytes_list() == [
                                _payload(len(admitted_requests))
                            ]
                            received.reply().message(b"reply-waiting").submit()
                        finally:
                            received.close()
                        _poll_until(poller, waiting_request.reply.done)

                        replies = await asyncio.gather(
                            *(submission.reply for submission in admitted_requests),
                            waiting_request.reply,
                        )
                        try:
                            assert replies[-1][0].to_bytes() == b"reply-waiting"
                        finally:
                            for parts in replies:
                                for part in parts:
                                    part.close()

    asyncio.run(exercise())
