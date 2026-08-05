package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.messaging.SubscriptionEntry;
import systems.zlink.contracts.messaging.SubscriptionEvent;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.contracts.sockets.XSubSocket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.List;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class SocketSubscriptionContractTest {
    @Test
    public void subscriptionHelpersRemainCanonicalWithoutSnapshotSurface() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             SubSocket sub = ctx.createSubSocket()) {
            sub.setSubscription("topic-a");
            sub.setSubscription("topic-b");
            assertEquals(2, sub.options().topicsCount());
            List<String> filters = List.of(
                sub.subscriptionAt(0).map(SubscriptionEntry::filter)
                    .orElseThrow(),
                sub.subscriptionAt(1).map(SubscriptionEntry::filter)
                    .orElseThrow());
            assertTrue(filters.contains("topic-a"));
            assertTrue(filters.contains("topic-b"));
            assertTrue(sub.subscriptionAt(2).isEmpty());
            sub.unsetSubscription("topic-b");
        }
    }

    @Test
    public void publishUsesCanonicalTopicPath() throws Exception {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             XPubSocket pub = ctx.createXPubSocket();
             SubSocket sub = ctx.createSubSocket();
             var pubMonitor = pub.monitorOpen(MonitorEventType.CONNECTION_READY);
             var subMonitor = sub.monitorOpen(MonitorEventType.CONNECTION_READY)) {
            String endpoint = TestSupport.inprocEndpoint("publish-contract");
            pub.bind(endpoint);
            sub.setSubscription("topic-a");
            sub.connect(endpoint);
            TestSupport.awaitMonitorEvent(subMonitor,
                MonitorEventType.CONNECTION_READY);
            TestSupport.awaitMonitorEvent(pubMonitor,
                MonitorEventType.CONNECTION_READY);

            SubscriptionEvent event = new SubscriptionEvent();
            assertTrue(pub.receiveSubscriptionEvent(event, RecvFlags.NONE));
            assertTrue(event.subscribed());
            assertEquals("topic-a", event.topic());

            try (Message part = Message.from("payload")) {
                pub.publish("topic-a").message(part).submit();
            }

            try (TopicMessage received = new TopicMessage()) {
                assertTrue(sub.subscribe(received, RecvFlags.NONE));
                assertEquals("topic-a", received.topic());
                assertArrayEquals("payload".getBytes(StandardCharsets.UTF_8),
                    received.singlePartOrThrow().toByteArray());
            }
        }
    }

    @Test
    public void subscribePullsTopicAwareMessage() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PubSocket pub = ctx.createPubSocket();
             SubSocket sub = ctx.createSubSocket();
             var pubMonitor = pub.monitorOpen(MonitorEventType.CONNECTION_READY);
             var subMonitor = sub.monitorOpen(MonitorEventType.CONNECTION_READY)) {
            String endpoint = TestSupport.inprocEndpoint("subscribe-contract");
            pub.bind(endpoint);
            sub.setSubscription("topic-b");
            sub.connect(endpoint);
            TestSupport.awaitMonitorEvent(subMonitor,
                MonitorEventType.CONNECTION_READY);
            TestSupport.awaitMonitorEvent(pubMonitor,
                MonitorEventType.CONNECTION_READY);

            try (Message part = Message.from("payload-b")) {
                pub.publish("topic-b").message(part).submit();
            }

            try (TopicMessage received = new TopicMessage()) {
                assertTrue(sub.subscribe(received, RecvFlags.NONE));
                assertEquals("topic-b", received.topic());
                assertArrayEquals("payload-b".getBytes(StandardCharsets.UTF_8),
                    received.singlePartOrThrow().toByteArray());
            }
        }
    }

    @Test
    public void xpubSubscriptionEventUsesDedicatedPubOptionSurface() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             XPubSocket pub = ctx.createXPubSocket();
             XSubSocket sub = ctx.createXSubSocket()) {
            pub.options().manual(true);
            String endpoint = TestSupport.inprocEndpoint("xpub-manual-contract");
            pub.bind(endpoint);
            sub.setSubscription("manual-topic");
            sub.connect(endpoint);

            SubscriptionEvent event = new SubscriptionEvent();
            assertTrue(pub.receiveSubscriptionEvent(event, RecvFlags.NONE));
            assertTrue(event.subscribed());
            assertEquals("manual-topic", event.topic());
        }
    }

    @Test
    public void subscriptionEventRecordShapeMatchesSpec() {
        assertNull(SubscriptionEvent.class.getRecordComponents());
        assertTrue(hasPublicMethod(SubscriptionEvent.class, "getRoutingId"));
        assertTrue(hasPublicMethod(SubscriptionEvent.class, "topic"));
        assertTrue(hasPublicMethod(SubscriptionEvent.class, "subscribed"));
    }

    private static boolean hasPublicMethod(Class<?> type, String name,
                                           Class<?>... parameterTypes) {
        try {
            type.getMethod(name, parameterTypes);
            return true;
        } catch (NoSuchMethodException ex) {
            return false;
        }
    }
}
