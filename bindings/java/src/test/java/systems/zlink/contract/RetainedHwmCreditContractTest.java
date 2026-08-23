/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.CoreHwmBudgetSnapshot;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubSocket;

final class RetainedHwmCreditContractTest {
    @Test
    void pairRetainedReceiveMovesAndReleasesOriginCredit() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket receiver = context.createPairSocket();
             PairSocket sender = context.createPairSocket()) {
            context.options().autoHwmEnabled(false);
            receiver.options().recvHwm(4_096L);
            sender.options().sendHwm(4_096L);
            String endpoint = TestSupport.inprocEndpoint(
                "java-retained-pair");
            receiver.bind(endpoint);
            sender.connect(endpoint);

            try (Message outbound = Message.allocate(1024)) {
                outbound.fill((byte) 0x6a);
                sender.send().message(outbound).submit()
                    .toCompletableFuture().join();
            }
            TestSupport.awaitCondition(() -> context.coreHwmBudgetSnapshot()
                .coreQueueAccountedBytes() > 0L);
            CoreHwmBudgetSnapshot queued = context.coreHwmBudgetSnapshot();
            assertEquals(0L, queued.applicationAccountedBytes());

            Received received = new Received();
            assertTrue(receiver.recvRetained(received, RecvFlags.NONE));
            assertEquals(1024, received.singlePartOrThrow().size());
            CoreHwmBudgetSnapshot retained =
                context.coreHwmBudgetSnapshot();
            assertEquals(0L, retained.coreQueueAccountedBytes());
            assertEquals(queued.coreQueueAccountedBytes(),
                retained.applicationAccountedBytes());
            assertEquals(queued.currentAccountedBytes(),
                retained.currentAccountedBytes());
            assertEquals(1L,
                retained.outstandingApplicationLeaseCount());

            Thread releaser = Thread.ofPlatform().start(received::close);
            releaser.join();
            TestSupport.awaitCondition(() -> context.coreHwmBudgetSnapshot()
                .outstandingApplicationLeaseCount() == 0L);
            assertEquals(0L, context.coreHwmBudgetSnapshot()
                .applicationAccountedBytes());

            try (Received reusable = new Received()) {
                try (Message first = Message.from("retained-first")) {
                    sender.send().message(first).submit()
                        .toCompletableFuture().join();
                }
                assertTrue(receiver.recvRetained(reusable, RecvFlags.NONE));
                try (Message second = Message.from("retained-second")) {
                    sender.send().message(second).submit()
                        .toCompletableFuture().join();
                }
                assertTrue(receiver.recvRetained(reusable, RecvFlags.NONE));
                assertEquals("retained-second",
                    reusable.singlePartOrThrow().toUtf8String());
                assertEquals(1L, context.coreHwmBudgetSnapshot()
                    .outstandingApplicationLeaseCount());
            }
            assertEquals(0L, context.coreHwmBudgetSnapshot()
                .outstandingApplicationLeaseCount());

            try (Message ordinary = Message.from("ordinary")) {
                sender.send().message(ordinary).submit()
                    .toCompletableFuture().join();
            }
            try (Received ordinary = new Received()) {
                assertTrue(receiver.recv(ordinary, RecvFlags.NONE));
            }
            assertEquals(0L, context.coreHwmBudgetSnapshot()
                .outstandingApplicationLeaseCount());
            try (Received empty = new Received()) {
                assertFalse(receiver.recvRetained(empty,
                    RecvFlags.DONT_WAIT));
            }
        }
    }

    @Test
    void dealerRetainedReceiveUsesTypedDealerPath() {
        TestSupport.assumeNative();

        RoutingId dealerRid = RoutingId.from("retained-dealer-recv");
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket()) {
            dealer.setRoutingId(dealerRid);
            String endpoint = TestSupport.inprocEndpoint(
                "java-retained-dealer-recv");
            router.bind(endpoint);
            dealer.connect(endpoint);

            try (Message ready = Message.from("ready")) {
                dealer.send().message(ready).submit()
                    .toCompletableFuture().join();
            }
            try (Received ready = new Received()) {
                assertTrue(router.recv(ready, RecvFlags.NONE));
            }
            try (Message payload = Message.from("dealer-payload")) {
                router.send(dealerRid).message(payload).submit()
                    .toCompletableFuture().join();
            }
            try (Received received = new Received()) {
                assertTrue(dealer.recvRetained(received, RecvFlags.NONE));
                assertEquals("dealer-payload",
                    received.singlePartOrThrow().toUtf8String());
                assertTrue(received.requestSeq().isEmpty());
                assertEquals(1L, context.coreHwmBudgetSnapshot()
                    .outstandingApplicationLeaseCount());
            }
            assertEquals(0L, context.coreHwmBudgetSnapshot()
                .outstandingApplicationLeaseCount());
        }
    }

    @Test
    void routerRetainedReceivePreservesMultipartRid()
        throws Exception {
        TestSupport.assumeNative();

        RoutingId dealerRid = RoutingId.from("retained-dealer");
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket()) {
            dealer.setRoutingId(dealerRid);
            String endpoint = TestSupport.inprocEndpoint(
                "java-retained-router");
            router.bind(endpoint);
            dealer.connect(endpoint);

            try (Message first = Message.from("one");
                 Message second = Message.from("two")) {
                dealer.send().message(first).message(second)
                    .submit().toCompletableFuture().join();
            }
            try (Received multipart = new Received()) {
                assertTrue(router.recvRetained(multipart, RecvFlags.NONE));
                assertArrayEquals(dealerRid.toBytes(), multipart.getRoutingId()
                    .orElseThrow().toBytes());
                assertTrue(multipart.requestSeq().isEmpty());
                assertEquals(List.of("one", "two"), multipart.parts().stream()
                    .map(Message::toUtf8String).toList());
                assertEquals(2L, context.coreHwmBudgetSnapshot()
                    .outstandingApplicationLeaseCount());
            }
            assertEquals(0L, context.coreHwmBudgetSnapshot()
                .outstandingApplicationLeaseCount());

        }
    }

    @Test
    void routerRetainedReceivePreservesRequestSequence() throws Exception {
        TestSupport.assumeNative();

        RoutingId dealerRid = RoutingId.from("retained-request-dealer");
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket()) {
            dealer.setRoutingId(dealerRid);
            String endpoint = TestSupport.inprocEndpoint(
                "java-retained-router-request");
            router.bind(endpoint);
            dealer.connect(endpoint);

            var reply = dealer.request()
                .message(Message.from("request"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit();
            try (Received request = new Received()) {
                assertTrue(router.recvRetained(request, RecvFlags.NONE));
                assertTrue(request.requestSeq().isPresent());
                assertArrayEquals(dealerRid.toBytes(), request.getRoutingId()
                    .orElseThrow().toBytes());
                request.reply().message(Message.from("reply")).submit();
                assertEquals(1L, context.coreHwmBudgetSnapshot()
                    .outstandingApplicationLeaseCount());
            }
            List<Message> replyParts = reply.toCompletableFuture().get(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            try {
                assertEquals("reply", replyParts.getFirst().toUtf8String());
            } finally {
                Message.closeAll(replyParts);
            }
            assertEquals(0L, context.coreHwmBudgetSnapshot()
                .outstandingApplicationLeaseCount());
            TestSupport.allowTcpRequestReplyCallbackHandshakeToSettle();
        }
    }

    @Test
    void retainedSubscribePreservesTopicAndOneLeasePerPhysicalPart() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PubSocket publisher = context.createPubSocket();
             SubSocket subscriber = context.createSubSocket();
             var pubMonitor = publisher.monitorOpen(
                 MonitorEventType.CONNECTION_READY);
             var subMonitor = subscriber.monitorOpen(
                 MonitorEventType.CONNECTION_READY)) {
            String endpoint = TestSupport.inprocEndpoint(
                "java-retained-subscribe");
            publisher.bind(endpoint);
            subscriber.setSubscription("orders");
            subscriber.connect(endpoint);
            TestSupport.awaitMonitorEvent(subMonitor,
                MonitorEventType.CONNECTION_READY);
            TestSupport.awaitMonitorEvent(pubMonitor,
                MonitorEventType.CONNECTION_READY);

            try (Message first = Message.from("alpha");
                 Message second = Message.from("beta")) {
                publisher.publish("orders").message(first)
                    .message(second).submit();
            }
            try (TopicMessage received = new TopicMessage()) {
                assertTrue(subscriber.subscribeRetained(received,
                    RecvFlags.NONE));
                assertEquals("orders", received.topic());
                assertEquals(List.of("alpha", "beta"),
                    received.parts().stream().map(Message::toUtf8String)
                        .toList());
                assertEquals(2L, context.coreHwmBudgetSnapshot()
                    .outstandingApplicationLeaseCount());
            }
            assertEquals(0L, context.coreHwmBudgetSnapshot()
                .outstandingApplicationLeaseCount());
        }
    }
}
