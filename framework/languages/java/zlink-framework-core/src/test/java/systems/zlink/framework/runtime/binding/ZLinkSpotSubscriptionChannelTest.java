package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;

import java.util.List;

final class ZLinkSpotSubscriptionChannelTest {
    @Test
    void runtimeTopicIsDeliveredOnlyOnItsRegisteredChannel() throws Exception {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.addChannel("events");
            node.addChannel("other");
            node.setRoutingId(RoutingId.from("subscription-node"));
            node.setBind("inproc://subscription-" + java.util.UUID.randomUUID());
            node.start();
            var spots = (ZLinkJavaRawSpotNode) node.spotNode();
            var target = (ZLinkJavaRawSpot) spots.createSpot("target");
            String topic = "orders." + target.lifecycleGeneration();
            var invalidChannel =
                    assertThrows(
                            systems.zlink.framework.errors.ZLinkConfigurationException.class,
                            () -> target.setSubscription("missing", topic));
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    invalidChannel.kind());
            target.setSubscription("events", topic);
            try (var part = Message.from("payload")) {
                spots.enqueueLogicalMulticast(
                        "other", topic, RoutingId.from("source"), new byte[0], null, List.of(part));
                var wrongChannel = target.subscribe(ZLinkBackendRecvMode.DONT_WAIT);
                if (wrongChannel != null) wrongChannel.parts().forEach(Message::close);
                assertNull(wrongChannel);
                spots.enqueueLogicalMulticast(
                        "events",
                        topic,
                        RoutingId.from("source"),
                        new byte[0],
                        null,
                        List.of(part));
                var received = target.subscribe(ZLinkBackendRecvMode.DONT_WAIT);
                assertNotNull(received);
                try {
                    assertEquals("events", received.channelName());
                    assertEquals(topic, received.topic());
                } finally {
                    received.parts().forEach(Message::close);
                }
            }
        }
    }
}
