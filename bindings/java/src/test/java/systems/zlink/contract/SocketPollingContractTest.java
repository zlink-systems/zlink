package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.eventing.PollEvent;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.PollSourceKind;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.contracts.sockets.StreamSocket;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class SocketPollingContractTest {
    @Test
    public void pollerTracksTypedSocketsThroughAbstractBase() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PairSocket server = ctx.createPairSocket();
             PairSocket client = ctx.createPairSocket();
             Poller poller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint("poller-contract");
            server.bind(endpoint);
            client.connect(endpoint);
            poller.add(server, 7L, PollEventFlags.POLLIN);

            try (Message outbound = Message.from("poller")) {
                client.send().message(outbound).submit();
            }

            PollEvents events = new PollEvents(4);
            int count = poller.wait(events,
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS));
            assertEquals(1, count);
            assertEquals(1, events.readyCount());
            assertEquals(1, poller.size());
            assertEquals(PollSourceKind.SOCKET, events.sourceKind(0));
            assertEquals(7L, events.slot(0));
            assertTrue(events.hasEvent(0, PollEventFlags.POLLIN));
            PollEvent event = events.eventAt(0);
            assertEquals(PollSourceKind.SOCKET, event.sourceKind());
            assertEquals(7L, event.slot());
            assertTrue((event.revents() & PollEventFlags.POLLIN.mask()) != 0);
        }
    }

    @Test
    public void pollerTracksRouterSocketReadableRecord() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             RouterSocket server = ctx.createRouterSocket();
             DealerSocket client = ctx.createDealerSocket();
             Poller poller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint("router-poller-contract");
            server.bind(endpoint);
            client.connect(endpoint);
            poller.add(server, 8L, PollEventFlags.POLLIN);

            try (Message outbound = Message.from("router-poller")) {
                client.send().message(outbound).submit();
            }

            PollEvents events = new PollEvents(1);
            assertEquals(1, poller.wait(events,
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
            assertTrue(events.hasEvent(0, PollEventFlags.POLLIN));

            try (Received received = new Received()) {
                assertTrue(server.recv(received, RecvFlags.DONT_WAIT));
                assertEquals("router-poller", received.parts().getFirst().toUtf8String());
            }
        }
    }

    @Test
    public void completionPollerOwnsDealerRequestProgress() throws Exception {
        TestSupport.assumeNative();

        CountDownLatch completed = new CountDownLatch(1);
        AtomicReference<RequestResult> result = new AtomicReference<>();
        AtomicReference<byte[]> replyPayload = new AtomicReference<>();
        AtomicReference<Throwable> callbackFailure = new AtomicReference<>();

        try (Context ctx = Zlink.createContext();
             RouterSocket server = ctx.createRouterSocket();
             DealerSocket client = ctx.createDealerSocket();
             Poller serverPoller = Zlink.createPoller();
             Poller clientPoller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint("poller-completion-owner");
            client.setRoutingId(RoutingId.from("completion-client"));
            server.bind(endpoint);
            client.connect(endpoint);
            serverPoller.add(server, 18L, PollEventFlags.POLLIN);

            try (Message request = Message.from("completion-request")) {
                client.request()
                    .message(request)
                    .flags(SendFlags.DONT_WAIT)
                    .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                    .submit((requestResult, received) -> {
                        try {
                            result.set(requestResult);
                            if (requestResult == RequestResult.OK
                                && !received.isEmpty()) {
                                replyPayload.set(received.getFirst().toByteArray());
                            }
                        } catch (Throwable failure) {
                            callbackFailure.set(failure);
                        } finally {
                            Message.closeAll(received);
                            completed.countDown();
                        }
                        });
            }

            // The request starts the binding fallback progress pump first.
            // Registering the public completion poller afterwards must transfer
            // ownership without leaving two completion consumers on the socket.
            clientPoller.add(client, 19L,
                PollEventFlags.POLLIN, PollEventFlags.POLLCOMPLETION);

            PollEvents serverEvents = new PollEvents(1);
            assertEquals(1, serverPoller.wait(serverEvents,
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
            try (Received received = new Received()) {
                assertTrue(server.recv(received, RecvFlags.DONT_WAIT));
                try (Message reply = Message.from("completion-reply")) {
                    received.reply().message(reply).submit();
                }
            }

            PollEvents clientEvents = new PollEvents(1);
            long deadline = System.nanoTime()
                + TimeUnit.MILLISECONDS.toNanos(TestSupport.DEFAULT_TIMEOUT_MS);
            while (completed.getCount() != 0 && System.nanoTime() < deadline) {
                clientPoller.wait(clientEvents, Duration.ofMillis(25));
            }
            assertTrue(completed.await(0, TimeUnit.MILLISECONDS),
                "public POLLIN|POLLCOMPLETION poller did not dispatch request callback");
            assertNull(callbackFailure.get(), "request callback raised");
            assertEquals(RequestResult.OK, result.get());
            assertEquals("completion-reply",
                new String(replyPayload.get(), java.nio.charset.StandardCharsets.UTF_8));
        }
    }

    @Test
    public void pollerTracksStreamSocketReadableRawRecord() throws Exception {
        TestSupport.assumeNative();

        String endpoint = TestSupport.tcpEndpoint();
        int port = Integer.parseInt(endpoint.substring(endpoint.lastIndexOf(':') + 1));
        byte[] payload = "stream-poller".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] rawRecord = ByteBuffer.allocate(6 + payload.length)
            .putShort((short) 0)
            .putInt(payload.length)
            .put(payload)
            .array();

        try (Context ctx = Zlink.createContext();
             StreamSocket server = ctx.createStreamSocket();
             Poller poller = Zlink.createPoller();
             java.net.Socket client = new java.net.Socket()) {
            server.bind(endpoint);
            poller.add(server, 17L, PollEventFlags.POLLIN);
            client.connect(new InetSocketAddress("127.0.0.1", port),
                TestSupport.DEFAULT_TIMEOUT_MS);
            client.getOutputStream().write(rawRecord);
            client.getOutputStream().flush();

            PollEvents events = new PollEvents(1);
            assertEquals(1, poller.wait(events,
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
            assertEquals(PollSourceKind.SOCKET, events.sourceKind(0));
            assertEquals(17L, events.slot(0));
            assertTrue(events.hasEvent(0, PollEventFlags.POLLIN));

            try (Received received = new Received()) {
                assertTrue(server.recv(received, RecvFlags.DONT_WAIT));
                assertTrue(received.parts().stream()
                    .mapToInt(Message::size)
                    .sum() >= rawRecord.length);
            }
        }
    }

    @Test
    public void pollerTracksTimersThroughCoreTimerRegistration() {
        TestSupport.assumeNative();

        try (ZlinkTimer timer = Zlink.createTimer();
             Poller poller = Zlink.createPoller()) {
            poller.add(timer, 11L);
            timer.start(Duration.ofMillis(1), 1L);

            PollEvents events = new PollEvents(4);
            int count = poller.wait(events,
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS));
            assertEquals(1, count);
            assertEquals(1, poller.size());
            assertEquals(PollSourceKind.TIMER, events.sourceKind(0));
            assertEquals(11L, events.slot(0));
            assertTrue(timer.recv() > 0L);
        }
    }

    @Test
    public void pollerRejectsEmptyReusableEventBuffer() {
        assertThrows(IllegalArgumentException.class, () -> new PollEvents(0));
    }

    @Test
    public void pollerCapacityLeavesRemainingReadySource() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PairSocket sender1 = ctx.createPairSocket();
             PairSocket receiver1 = ctx.createPairSocket();
             PairSocket sender2 = ctx.createPairSocket();
             PairSocket receiver2 = ctx.createPairSocket();
             Poller poller = Zlink.createPoller()) {
            String endpoint1 = TestSupport.inprocEndpoint("poller-capacity-a");
            String endpoint2 = TestSupport.inprocEndpoint("poller-capacity-b");
            sender1.bind(endpoint1);
            receiver1.connect(endpoint1);
            sender2.bind(endpoint2);
            receiver2.connect(endpoint2);
            poller.add(receiver1, 101L, PollEventFlags.POLLIN);
            poller.add(receiver2, 102L, PollEventFlags.POLLIN);

            try (Message a = Message.from("a");
                 Message b = Message.from("b")) {
                sender1.send().message(a).submit();
                sender2.send().message(b).submit();
            }

            PollEvents events = new PollEvents(1);
            int count = poller.wait(events,
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS));
            assertEquals(1, count);
            long firstSlot = events.slot(0);
            assertTrue(firstSlot == 101L || firstSlot == 102L);
            if (firstSlot == 101L)
                assertEquals("a", recvUtf8(receiver1));
            else
                assertEquals("b", recvUtf8(receiver2));

            count = poller.wait(events,
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS));
            assertEquals(1, count);
            assertTrue(events.slot(0) == 101L || events.slot(0) == 102L);
            assertFalse(events.slot(0) == firstSlot);
            if (events.slot(0) == 101L)
                assertEquals("a", recvUtf8(receiver1));
            else
                assertEquals("b", recvUtf8(receiver2));
        }
    }

    @Test
    public void pollerModifyRemoveAndTimeoutFollowCoreSemantics() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PairSocket sender = ctx.createPairSocket();
             PairSocket receiver = ctx.createPairSocket();
             Poller poller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint("poller-modify-remove");
            sender.bind(endpoint);
            receiver.connect(endpoint);
            poller.add(receiver, 31L, PollEventFlags.POLLIN);
            poller.modify(receiver);

            try (Message hidden = Message.from("hidden")) {
                sender.send().message(hidden).submit();
            }

            PollEvents events = new PollEvents(1);
            assertEquals(0, poller.wait(events, Duration.ofMillis(20)));

            poller.modify(receiver, PollEventFlags.POLLIN);
            assertEquals(1, poller.wait(events,
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
            assertEquals(31L, events.slot(0));
            assertEquals("hidden", recvUtf8(receiver));

            assertTrue(poller.remove(receiver));
            try (Message removed = Message.from("removed")) {
                sender.send().message(removed).submit();
            }
            assertEquals(0, poller.wait(events, Duration.ZERO));
        }
    }

    @Test
    public void pollerDistinguishesTimerAndSocketInSameBuffer() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PairSocket sender = ctx.createPairSocket();
             PairSocket receiver = ctx.createPairSocket();
             ZlinkTimer timer = Zlink.createTimer();
             Poller poller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint("poller-timer-socket");
            sender.bind(endpoint);
            receiver.connect(endpoint);
            poller.add(receiver, 41L, PollEventFlags.POLLIN);
            poller.add(timer, 42L);

            try (Message socket = Message.from("socket")) {
                sender.send().message(socket).submit();
            }
            timer.start(Duration.ofMillis(5), 1L);

            PollEvents events = new PollEvents(2);
            boolean sawSocket = false;
            boolean sawZlinkTimer = false;
            long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while ((!sawSocket || !sawZlinkTimer) && System.nanoTime() < deadline) {
                int count = poller.wait(events, Duration.ofMillis(200));
                for (int i = 0; i < count; i++) {
                    if (events.sourceKind(i) == PollSourceKind.SOCKET) {
                        assertEquals(41L, events.slot(i));
                        assertEquals("socket", recvUtf8(receiver));
                        sawSocket = true;
                    } else if (events.sourceKind(i) == PollSourceKind.TIMER) {
                        assertEquals(42L, events.slot(i));
                        assertEquals(1L, timer.recv());
                        sawZlinkTimer = true;
                    }
                }
            }

            assertTrue(sawSocket);
            assertTrue(sawZlinkTimer);
        }
    }

    @Test
    public void pollerSurfaceDoesNotExposeAllocationWaitOrObjectTags()
        throws Exception {
        assertFalse(hasPublicMethod(Poller.class, "wait", Duration.class));
        assertFalse(hasPublicMethod(Poller.class, "wait", int.class,
            Duration.class));
        assertFalse(hasPublicMethod(Poller.class, "wait", List.class,
            Duration.class));
        assertFalse(hasPublicMethod(Poller.class, "add", ZlinkTimer.class,
            Object.class));
        assertFalse(hasPublicMethod(Poller.class, "add", Socket.class,
            Object.class, PollEventFlags[].class));
    }

    @Test
    public void legacySocketPollSetTypeIsNotPartOfTheRootPackage() {
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName("systems.zlink.contracts.sockets.SocketPollSet"));
    }

    private static boolean hasPublicMethod(Class<?> type, String name,
                                           Class<?>... parameters) {
        try {
            type.getMethod(name, parameters);
            return true;
        } catch (NoSuchMethodException ex) {
            return false;
        }
    }

    private static String recvUtf8(PairSocket socket) {
        try (Received received = new Received()) {
            assertTrue(socket.recv(received, RecvFlags.NONE));
            return received.singlePartOrThrow().toUtf8String();
        }
    }
}
