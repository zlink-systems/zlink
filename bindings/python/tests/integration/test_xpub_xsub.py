import unittest

import zlink

from .helpers import (
    transports,
    endpoint_for,
    try_transport,
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
