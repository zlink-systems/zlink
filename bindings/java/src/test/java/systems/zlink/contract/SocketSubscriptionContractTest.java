package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.messaging.SubscriptionEntry;
import systems.zlink.contracts.messaging.SubscriptionEvent;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.contracts.sockets.XSubSocket;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.time.Duration;
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

    // Core reports a short query buffer as CONFIG_BUFFER_TOO_SMALL (ENOBUFS)
    // with the needed length; the binding grows on that result.
    @Test
    public void subscriptionAtGrowsOnCoreBufferTooSmall() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             SubSocket sub = ctx.createSubSocket()) {
            String filter = "f".repeat(200);
            sub.setSubscription(filter);
            assertEquals(filter, sub.subscriptionAt(0)
                .map(SubscriptionEntry::filter).orElseThrow());
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
    public void subscribeReceives300ByteTopic() {
        assertSubscribeReceivesTopic(300);
    }

    @Test
    public void subscribeReceives70000ByteTopic() {
        assertSubscribeReceivesTopic(70_000);
    }

    private static void assertSubscribeReceivesTopic(int topicLength) {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             XPubSocket pub = ctx.createXPubSocket();
             SubSocket sub = ctx.createSubSocket();
             var pubMonitor = pub.monitorOpen(MonitorEventType.CONNECTION_READY);
             var subMonitor = sub.monitorOpen(MonitorEventType.CONNECTION_READY);
             Poller poller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint(
                "subscribe-long-topic-contract");
            pub.bind(endpoint);
            sub.setSubscription("t");
            sub.connect(endpoint);
            TestSupport.awaitMonitorEvent(subMonitor,
                MonitorEventType.CONNECTION_READY);
            TestSupport.awaitMonitorEvent(pubMonitor,
                MonitorEventType.CONNECTION_READY);
            poller.add(sub, 1L, PollEventFlags.POLLIN);

            SubscriptionEvent subscription = new SubscriptionEvent();
            assertTrue(receiveSubscriptionEventOrReport(pub, subscription,
                RecvFlags.NONE));
            assertTrue(subscription.subscribed());
            assertEquals("t", subscription.topic());

            String topic = "t" + "x".repeat(topicLength - 1);
            for (int index = 0; index < 2; index++) {
                try (Message part = Message.from("payload")) {
                    pub.publish(topic).message(part).submit();
                }
            }

            RecvFlags firstFlags = topicLength == 70_000
                ? RecvFlags.DONT_WAIT : RecvFlags.NONE;
            RecvFlags secondFlags = topicLength == 70_000
                ? RecvFlags.NONE : RecvFlags.DONT_WAIT;
            RecvFlags[] flags = {firstFlags, secondFlags};
            PollEvents events = new PollEvents(1);
            for (RecvFlags receiveFlags : flags) {
                awaitReadable(poller, events);
                try (TopicMessage received = new TopicMessage()) {
                    assertTrue(subscribeOrReport(sub, received, receiveFlags));
                    assertEquals(topic, received.topic());
                    assertArrayEquals("payload".getBytes(StandardCharsets.UTF_8),
                        received.singlePartOrThrow().toByteArray());
                }
            }
        }
    }

    @Test
    public void subscriptionEventReceives300ByteTopic() {
        assertSubscriptionEventReceivesTopic(300);
    }

    @Test
    public void subscriptionEventReceives70000ByteTopic() {
        assertSubscriptionEventReceivesTopic(70_000);
    }

    private static void assertSubscriptionEventReceivesTopic(int topicLength) {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             XPubSocket pub = ctx.createXPubSocket();
             XSubSocket sub = ctx.createXSubSocket();
             var pubMonitor = pub.monitorOpen(MonitorEventType.CONNECTION_READY);
             var subMonitor = sub.monitorOpen(MonitorEventType.CONNECTION_READY);
             Poller poller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint(
                "xpub-long-topic-contract");
            pub.bind(endpoint);
            sub.connect(endpoint);
            TestSupport.awaitMonitorEvent(subMonitor,
                MonitorEventType.CONNECTION_READY);
            TestSupport.awaitMonitorEvent(pubMonitor,
                MonitorEventType.CONNECTION_READY);
            poller.add(pub, 1L, PollEventFlags.POLLIN);

            String topic = "t".repeat(topicLength);
            String secondTopic = "s" + "x".repeat(topicLength - 1);
            sub.setSubscription(topic);
            sub.setSubscription(secondTopic);

            RecvFlags firstFlags = topicLength == 70_000
                ? RecvFlags.DONT_WAIT : RecvFlags.NONE;
            RecvFlags secondFlags = topicLength == 70_000
                ? RecvFlags.NONE : RecvFlags.DONT_WAIT;
            String[] expectedTopics = {topic, secondTopic};
            RecvFlags[] flags = {firstFlags, secondFlags};
            PollEvents events = new PollEvents(1);
            for (int index = 0; index < expectedTopics.length; index++) {
                awaitReadable(poller, events);
                SubscriptionEvent event = new SubscriptionEvent();
                assertTrue(receiveSubscriptionEventOrReport(pub, event,
                    flags[index]));
                assertTrue(event.subscribed());
                assertEquals(expectedTopics[index], event.topic());
            }
        }
    }

    private static void awaitReadable(Poller poller, PollEvents events) {
        assertEquals(1, poller.wait(events,
            Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
        assertTrue(events.hasEvent(0, PollEventFlags.POLLIN));
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

    private static boolean subscribeOrReport(SubSocket sub,
                                             TopicMessage received,
                                             RecvFlags flags) {
        try {
            return sub.subscribe(received, flags);
        } catch (ZlinkRecvException ex) {
            throw new AssertionError(
                "SUB receive returned " + ex.getResult(), ex);
        }
    }

    private static boolean receiveSubscriptionEventOrReport(
        XPubSocket pub, SubscriptionEvent event, RecvFlags flags) {
        try {
            return pub.receiveSubscriptionEvent(event, flags);
        } catch (ZlinkRecvException ex) {
            throw new AssertionError(
                "XPUB subscription receive returned " + ex.getResult(), ex);
        }
    }

}
