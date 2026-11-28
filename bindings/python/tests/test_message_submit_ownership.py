# SPDX-License-Identifier: MPL-2.0

import asyncio
import concurrent.futures

import pytest
import zlink

from completion_poller import CompletionPollerDriver


def assert_consumed(message):
    assert message.size() == 0
    assert bytes(message.data) == b""
    assert message.to_bytes() == b""


@pytest.mark.parametrize("managed", [False, True])
def test_send_consumes_message_after_admission(managed):
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_pair_socket(context) as sender,
            zlink.create_pair_socket(context) as receiver,
            zlink.create_received() as received,
            zlink.Message.from_(b"send") as message,
            message.copy() as saved,
        ):
            sender.bind("inproc://message-send-ownership")
            receiver.connect("inproc://message-send-ownership")
            receiver.send().message(b"ready").submit_sync()
            assert sender.recv_into(received)

            operation = sender.send().messages(message, b"")
            if managed:
                submission = operation.submit()
                assert submission.result is zlink.SubmitResult.OK
                assert_consumed(message)
                await submission.admitted
            else:
                operation.submit_sync()
                assert_consumed(message)
            assert saved.to_bytes() == b"send"
            assert receiver.recv_into(received)
            assert received.to_bytes_list() == [b"send", b""]

    asyncio.run(exercise())


@pytest.mark.parametrize("request_mode", ["sync", "async"])
def test_request_and_reply_consume_messages_after_admission(request_mode):
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_dealer_socket(context) as client,
            zlink.create_router_socket(context) as server,
            zlink.Message.from_(b"request") as request,
            zlink.Message.from_(b"reply") as reply,
            zlink.Message.from_(b"rejected reply") as rejected,
            concurrent.futures.ThreadPoolExecutor(1) as worker,
        ):
            endpoint = "inproc://message-request-ownership"
            client.options.receive_timeout_ms = 2000
            server.options.receive_timeout_ms = 2000
            server.bind(endpoint)
            client.connect(endpoint)
            client.send().message(b"ready").submit_sync()
            with zlink.create_received() as ready:
                assert server.recv_into(ready)

            def serve():
                with zlink.create_received() as received:
                    assert server.recv_into(received)
                    assert received.to_bytes_list() == [b"request"]
                    received.reply().message(reply).submit()
                    with pytest.raises(zlink.SubmitError):
                        received.reply().message(rejected).submit()

            served = worker.submit(serve)
            operation = client.request().message(request).timeout(2)
            if request_mode == "async":
                with CompletionPollerDriver(client):
                    submission = operation.submit()
                    assert submission.result is zlink.SubmitResult.OK
                    assert_consumed(request)
                    result = await submission.reply
            else:
                result = operation.submit_sync()
            try:
                assert [part.to_bytes() for part in result] == [b"reply"]
                served.result(timeout=3)
                assert rejected.to_bytes() == b"rejected reply"
                assert_consumed(request)
                assert_consumed(reply)
            finally:
                for part in result:
                    part.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("operation", ["send", "request"])
@pytest.mark.parametrize("managed", [False, True])
def test_synchronous_rejection_preserves_message(operation, managed):
    async def exercise():
        with (
            zlink.create_context() as context,
            zlink.create_router_socket(context) as router,
            zlink.Message.from_(b"rejected") as message,
        ):
            router.options.send_timeout_ms = 0
            rid = zlink.RoutingId.from_(b"missing")
            builder = getattr(router, operation)(rid).message(message)
            if managed:
                with CompletionPollerDriver(router), pytest.raises(zlink.SubmitError):
                    builder.submit()
            else:
                with pytest.raises(zlink.SubmitError):
                    builder.submit_sync()
            assert message.to_bytes() == b"rejected"

    asyncio.run(exercise())


def test_publish_consumes_message_on_success_and_preserves_on_rejection():
    with (
        zlink.create_context() as context,
        zlink.create_pub_socket(context) as publisher,
        zlink.Message.from_(b"publish") as message,
    ):
        with pytest.raises(ValueError):
            publisher.publish("invalid\x00topic").message(message).submit()
        assert message.to_bytes() == b"publish"
        publisher.publish("topic").messages(message, b"").submit()
        assert_consumed(message)
