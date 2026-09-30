import unittest
from unittest.mock import patch

import zlink

from .helpers import (
    transports,
    endpoint_for,
    try_transport,
    wait_connected,
)

class XpubXsubScenarioTest(unittest.TestCase):
    def test_xpub_xsub_subscription(self):
        ctx = zlink.create_context()
        for name, endpoint in transports("xpub"):
            def run():
                xpub = zlink.create_xpub_socket(ctx)
                xsub = zlink.create_xsub_socket(ctx)
                xpub.pub_options.verbose = True
                ep = endpoint_for(name, endpoint, "-xpub")
                xpub.bind(ep)
                xsub.connect(ep)
                xsub.set_subscription(b"topic")
                event = zlink.create_subscription_event()
                self.assertTrue(xpub.receive_subscription_event_into(event))
                self.assertTrue(event.subscribed)
                self.assertEqual(event.topic, "topic")
                xpub.close()
                xsub.close()
            try_transport(name, run)
        ctx.close()

    def test_xpub_xsub_long_topic_subscription_event(self):
        ctx = zlink.create_context()
        xpub = zlink.create_xpub_socket(ctx)
        xsub = zlink.create_xsub_socket(ctx)
        try:
            xpub.pub_options.verbose = True
            endpoint = next(
                endpoint
                for name, endpoint in transports("xpub-long-topic-event")
                if name == "inproc"
            )
            xpub.bind(endpoint)
            xsub.connect(endpoint)
            topic = b"t" * 300
            xsub.set_subscription(topic)

            event = zlink.create_subscription_event()
            self.assertTrue(xpub.receive_subscription_event_into(event))
            self.assertTrue(event.subscribed)
            self.assertEqual(event.topic, topic.decode("ascii"))
        finally:
            xpub.close()
            xsub.close()
            ctx.close()

    def test_xpub_xsub_long_topic_message(self):
        ctx = zlink.create_context()
        publisher = zlink.create_pub_socket(ctx)
        subscriber = zlink.create_sub_socket(ctx)
        topic = b"t" * 300
        received = zlink.create_topic_message()
        try:
            endpoint = next(
                endpoint
                for name, endpoint in transports("pubsub-long-topic-message")
                if name == "inproc"
            )
            with publisher.monitor_open(
                zlink.MonitorEventMask.CONNECTION_READY
            ) as publisher_monitor:
                with subscriber.monitor_open(
                    zlink.MonitorEventMask.CONNECTION_READY
                ) as subscriber_monitor:
                    publisher.bind(endpoint)
                    subscriber.connect(endpoint)
                    subscriber.set_subscription(topic)
                    wait_connected(publisher_monitor, subscriber_monitor)

            publisher.publish(topic).message(b"payload").submit()
            self.assertTrue(subscriber.subscribe_into(received))
            self.assertEqual(received.topic, topic.decode("ascii"))
            self.assertEqual(received.to_bytes_list(), [b"payload"])
        finally:
            received.close()
            publisher.close()
            subscriber.close()
            ctx.close()

    def test_xpub_xsub_long_topic_message_python_fallback(self):
        ctx = zlink.create_context()
        publisher = zlink.create_pub_socket(ctx)
        subscriber = zlink.create_sub_socket(ctx)
        topic = b"t" * 300
        received = zlink.create_topic_message()
        try:
            endpoint = next(
                endpoint
                for name, endpoint in transports("pubsub-long-topic-fallback")
                if name == "inproc"
            )
            with publisher.monitor_open(
                zlink.MonitorEventMask.CONNECTION_READY
            ) as publisher_monitor:
                with subscriber.monitor_open(
                    zlink.MonitorEventMask.CONNECTION_READY
                ) as subscriber_monitor:
                    publisher.bind(endpoint)
                    subscriber.connect(endpoint)
                    subscriber.set_subscription(topic)
                    wait_connected(publisher_monitor, subscriber_monitor)

            publisher.publish(topic).message(b"payload").submit()
            with patch(
                "zlink._runtime.sockets.socket_base._native_extension", None
            ), patch(
                "zlink._runtime.sockets.socket_base._native_subscribe_owner", None
            ):
                self.assertTrue(subscriber.subscribe_into(received))
            self.assertEqual(received.topic, topic.decode("ascii"))
            self.assertEqual(received.to_bytes_list(), [b"payload"])
        finally:
            received.close()
            publisher.close()
            subscriber.close()
            ctx.close()
