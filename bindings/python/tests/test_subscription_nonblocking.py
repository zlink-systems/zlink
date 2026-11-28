# SPDX-License-Identifier: MPL-2.0

import pytest
import zlink


@pytest.mark.parametrize("factory", [zlink.create_sub_socket, zlink.create_xsub_socket])
def test_nonblocking_subscription_receives_only_ready_records(factory):
    flags = zlink.RecvFlags.DONT_WAIT
    with (
        zlink.create_context() as context,
        zlink.create_xpub_socket(context) as publisher,
        factory(context) as subscriber,
        zlink.create_topic_message() as message,
        zlink.create_poller() as poller,
    ):
        event = zlink.create_subscription_event()
        assert publisher.receive_subscription_event_into(event, flags=flags) is False
        assert subscriber.subscribe_into(message, flags=flags) is False

        endpoint = "inproc://nonblocking-subscription"
        publisher.bind(endpoint)
        subscriber.connect(endpoint)
        subscriber.set_subscription("topic")

        events = zlink.create_poll_events(1)
        poller.add_socket(publisher, zlink.PollEventFlag.POLLIN, 1)
        assert poller.wait(events, 1000) == 1
        assert publisher.receive_subscription_event_into(event, flags=flags) is True
        assert (event.topic, event.subscribed) == ("topic", True)
        assert publisher.receive_subscription_event_into(event, flags=flags) is False

        poller.remove_socket(publisher)
        poller.add_socket(subscriber, zlink.PollEventFlag.POLLIN, 2)
        publisher.publish("topic").messages(b"first", b"").submit()
        assert poller.wait(events, 1000) == 1
        assert subscriber.subscribe_into(message, flags=flags) is True
        assert message.topic == "topic"
        assert message.to_bytes_list() == [b"first", b""]
        assert subscriber.subscribe_into(message, flags=flags) is False
