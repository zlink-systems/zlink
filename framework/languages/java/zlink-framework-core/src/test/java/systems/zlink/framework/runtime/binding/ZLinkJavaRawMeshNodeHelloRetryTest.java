package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkJavaRawMeshNodeHelloRetryTest {
    private static final Duration IO_DEADLINE = Duration.ofSeconds(2);

    @Test
    void helloAwaitingAdmitUsesLivenessPeriodsRatherThanReceiveRotations() throws Exception {
        var now = new AtomicLong();
        var interval = Duration.ofSeconds(5);
        var localRid = RoutingId.from("hello-cadence-local");
        var remoteRid = RoutingId.from("hello-cadence-remote");
        var wire = new ZLinkServiceM6AWireCodec();
        try (var context = Zlink.createContext();
                var node =
                        new ZLinkJavaRawMeshNode(
                                context,
                                "hello-retry",
                                () -> 0L,
                                now::get,
                                interval,
                                Duration.ofSeconds(15));
                var peerPort = new ZLinkJavaRawServicePort(context);
                var peer = peerPort.openRouter(remoteRid)) {
            var port = (ZLinkJavaRawServicePort) field(node, "port");
            var router = port.openRouter(localRid);
            set(node, "router", router);
            set(node, "localDescriptor", descriptor(localRid));
            set(node, "topology", new ZLinkServiceTopologyRegistry(descriptor(localRid)));
            String endpoint = "inproc://hello-cadence-" + java.util.UUID.randomUUID();
            peer.bind(endpoint);
            node.connectPeer(endpoint, remoteRid);
            port.ensureReceivePollerRegistered(router);
            long deadline = System.nanoTime() + IO_DEADLINE.toNanos();
            while (port.routesSnapshot(router).isEmpty()) {
                assertTrue(System.nanoTime() < deadline, "Core route did not become selectable");
                port.waitForReady(router, IO_DEADLINE);
            }
            invoke(node, "observeSelectedRoutes");
            tick(node, now.get());
            var exchange = node.helloExchanges.get(remoteRid);
            exchange.submission().get(2, java.util.concurrent.TimeUnit.SECONDS);
            assertEquals(ServiceWireConstants.COMMAND_HELLO, receive(peerPort, peer, wire));
            int sent = 1;
            for (int period = 1; period <= 5; period++) {
                long due = interval.toNanos() * period;
                for (long time : new long[] {due - interval.toNanos() + 1, due - 1}) {
                    now.set(time);
                    tick(node, time);
                    assertSame(
                            exchange,
                            node.helloExchanges.get(remoteRid),
                            "a receive rotation before the management deadline must not submit"
                                    + " HELLO");
                    assertTrue(
                            node.managementDeadlineNanos(time) - time > 0,
                            "waiting for ADMIT must not pin the management deadline to now");
                }
                now.set(due);
                tick(node, due);
                var next = node.helloExchanges.get(remoteRid);
                assertNotSame(exchange, next, "the next liveness period must submit HELLO");
                next.submission().get(2, java.util.concurrent.TimeUnit.SECONDS);
                assertEquals(ServiceWireConstants.COMMAND_HELLO, receive(peerPort, peer, wire));
                sent++;
                exchange = next;
            }
            assertEquals(6, sent, "initial HELLO plus five liveness periods");
        }
    }

    @ParameterizedTest
    @CsvSource({"false,false", "true,false", "false,true", "true,true"})
    void eachLivenessPeriodResendsHelloUntilAdmitIncludingInboundRoutes(
            boolean withIntent, boolean pausePublication) throws Exception {
        var localRid = RoutingId.from("hello-retry-local");
        var remoteRid = RoutingId.from("hello-retry-remote");
        var wire = new ZLinkServiceM6AWireCodec();
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "hello-retry");
                var peerPort = new ZLinkJavaRawServicePort(context);
                var peer = peerPort.openRouter(remoteRid);
                var readyEvents = new PeerReadyEvents(remoteRid)) {
            var port = (ZLinkJavaRawServicePort) field(node, "port");
            var router = port.openRouter(localRid);
            String endpoint = "inproc://hello-retry-" + java.util.UUID.randomUUID();
            router.bind(endpoint);
            set(node, "router", router);
            set(node, "localDescriptor", descriptor(localRid));
            set(node, "topology", new ZLinkServiceTopologyRegistry(descriptor(localRid)));
            if (withIntent) {
                String peerEndpoint = descriptor(remoteRid, 2).advertisedEndpoint();
                peer.bind(peerEndpoint);
                node.connectPeer(peerEndpoint, remoteRid);
            } else {
                peer.options().probe(true);
                peer.connect(endpoint);
            }
            port.ensureReceivePollerRegistered(router);
            long deadline = System.nanoTime() + IO_DEADLINE.toNanos();
            while (port.routesSnapshot(router).isEmpty()) {
                assertTrue(System.nanoTime() < deadline, "Core route did not become selectable");
                port.waitForReady(router, IO_DEADLINE);
            }
            invoke(node, "observeSelectedRoutes");
            if (pausePublication) {
                var lane =
                        (systems.zlink.framework.runtime.internal.execution.ZLinkStateLane)
                                field(node, "descriptorStateLane");
                var entered = new java.util.concurrent.CompletableFuture<Void>();
                var release = new java.util.concurrent.CompletableFuture<Void>();
                lane.runAsync(
                        () -> {
                            entered.complete(null);
                            release.join();
                            return null;
                        });
                try {
                    entered.get(2, java.util.concurrent.TimeUnit.SECONDS);
                    tick(node, 0);
                    var queued = node.helloExchanges.get(remoteRid).submission();
                    tick(node, 1);
                    release.complete(null);
                    queued.get(2, java.util.concurrent.TimeUnit.SECONDS);
                    lane.runAsync(() -> null)
                            .toCompletableFuture()
                            .get(2, java.util.concurrent.TimeUnit.SECONDS);
                    assertEquals(ServiceWireConstants.COMMAND_HELLO, receive(peerPort, peer, wire));
                    port.send(
                                    router,
                                    remoteRid,
                                    List.of(
                                            wire.encodeAdmission(
                                                    ServiceWireConstants.COMMAND_UPDATE,
                                                    descriptor(localRid))))
                            .toCompletableFuture()
                            .get(2, java.util.concurrent.TimeUnit.SECONDS);
                    assertEquals(
                            ServiceWireConstants.COMMAND_UPDATE,
                            receive(peerPort, peer, wire),
                            "a queued HELLO must not be submitted twice before its completion");
                } finally {
                    release.complete(null);
                }
            }
            // No receive pump or wall-clock retry runs. Advance only the specified probe cadence.
            long firstPeriod = pausePublication ? Duration.ofSeconds(5).toNanos() : 0;
            tick(node, firstPeriod);
            var first = node.helloExchanges.get(remoteRid);
            if (first != null && first.submission() != null)
                first.submission().get(2, java.util.concurrent.TimeUnit.SECONDS);
            byte[] firstHello = receiveFrame(peerPort, peer);
            assertEquals(
                    ServiceWireConstants.COMMAND_HELLO, wire.decodeHeader(firstHello).command());
            // The peer is known locally, but our outbound HELLO still awaits ADMIT.
            // Publication must not overtake that exchange with a higher revision UPDATE.
            peerPort.send(
                            peer,
                            localRid,
                            List.of(
                                    wire.encodeAdmission(
                                            ServiceWireConstants.COMMAND_HELLO,
                                            descriptor(remoteRid, 2))))
                    .toCompletableFuture()
                    .get(2, java.util.concurrent.TimeUnit.SECONDS);
            dispatchNext(node, port, router);
            assertEquals(ServiceWireConstants.COMMAND_ADMIT, receive(peerPort, peer, wire));
            assertEquals(
                    0,
                    node.status().admittedPeerCount(),
                    "successful local ADMIT submission must not make the peer ready");
            assertEquals(0, readyEvents.count.get());
            assertFalse(node.isCanonicalRelocationTargetAdmitted(remoteRid, 1));
            assertFalse(node.observePeerRoute(remoteRid).ready());
            assertEquals(0, node.readyPeerCount());
            assertEquals(0, node.readyChannelMemberCount("orders"));
            assertTrue(node.selectPlacementTarget().isEmpty());
            try (var body = Message.from(new byte[] {1})) {
                assertNotReady(node.sendNode(remoteRid, null, List.of(body)));
                assertNotReady(
                        node.requestNode(
                                remoteRid, null, List.of(body), IO_DEADLINE, null, null, null));
                assertNotReady(node.sendChannel("orders", null, List.of(body)));
                assertNotReady(
                        node.requestChannel(
                                "orders", null, List.of(body), IO_DEADLINE, null, null, null));
            }
            var publish =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "publishLocalDescriptor", ZLinkServiceNodeDescriptor.class);
            publish.setAccessible(true);
            var lane =
                    (systems.zlink.framework.runtime.internal.execution.ZLinkStateLane)
                            field(node, "descriptorStateLane");
            lane.runAsync(
                            () -> {
                                try {
                                    publish.invoke(node, descriptor(localRid, 2));
                                } catch (ReflectiveOperationException failure) {
                                    throw new AssertionError(failure);
                                }
                                return null;
                            })
                    .toCompletableFuture()
                    .get(2, java.util.concurrent.TimeUnit.SECONDS);
            tick(node, firstPeriod + Duration.ofSeconds(5).toNanos());
            var second = node.helloExchanges.get(remoteRid);
            if (second != null && second.submission() != null)
                second.submission().get(2, java.util.concurrent.TimeUnit.SECONDS);
            assertArrayEquals(
                    firstHello,
                    receiveFrame(peerPort, peer),
                    "HELLO retries must retain the first descriptor until ADMIT");

            // A crossing HELLO must receive the same descriptor snapshot in ADMIT.
            peerPort.send(
                            peer,
                            localRid,
                            List.of(
                                    wire.encodeAdmission(
                                            ServiceWireConstants.COMMAND_HELLO,
                                            descriptor(remoteRid, 2))))
                    .toCompletableFuture()
                    .get(2, java.util.concurrent.TimeUnit.SECONDS);
            dispatchNext(node, port, router);
            byte[] reply = receiveFrame(peerPort, peer);
            assertEquals(
                    descriptor(localRid),
                    wire.decodeAdmission(reply, ServiceWireConstants.COMMAND_ADMIT, localRid));

            peerPort.send(
                            peer,
                            localRid,
                            List.of(
                                    wire.encodeAdmission(
                                            ServiceWireConstants.COMMAND_ADMIT,
                                            descriptor(remoteRid, 2))))
                    .toCompletableFuture()
                    .get(2, java.util.concurrent.TimeUnit.SECONDS);
            dispatchNext(node, port, router);
            assertEquals(1, node.status().admittedPeerCount());
            assertEquals(1, readyEvents.count.get());
            assertTrue(node.isCanonicalRelocationTargetAdmitted(remoteRid, 1));
            assertTrue(node.observePeerRoute(remoteRid).ready());
            assertEquals(1, node.readyPeerCount());
            assertEquals(1, node.readyChannelMemberCount("orders"));
            assertEquals(remoteRid, node.selectPlacementTarget().orElseThrow());
            byte[] update = receiveFrame(peerPort, peer);
            assertEquals(
                    descriptor(localRid, 2),
                    wire.decodeAdmission(update, ServiceWireConstants.COMMAND_UPDATE, localRid));
            peerPort.send(
                            peer,
                            localRid,
                            List.of(
                                    wire.encodeAdmission(
                                            ServiceWireConstants.COMMAND_ADMIT,
                                            descriptor(remoteRid, 2))))
                    .toCompletableFuture()
                    .get(2, java.util.concurrent.TimeUnit.SECONDS);
            dispatchNext(node, port, router);
            assertEquals(ServiceWireConstants.COMMAND_UPDATE, receive(peerPort, peer, wire));
            assertEquals(1, node.status().admittedPeerCount(), "duplicate ADMIT is idempotent");
            assertEquals(
                    1, readyEvents.count.get(), "duplicate ADMIT must not announce ready again");
            assertEquals(1, node.readyChannelMemberCount("orders"));
            try (var body = Message.from(new byte[] {1})) {
                node.sendNode(remoteRid, null, List.of(body))
                        .toCompletableFuture()
                        .get(2, java.util.concurrent.TimeUnit.SECONDS);
                assertEquals(ServiceWireConstants.COMMAND_NODE_SEND, receive(peerPort, peer, wire));
                node.sendChannel("orders", null, List.of(body))
                        .toCompletableFuture()
                        .get(2, java.util.concurrent.TimeUnit.SECONDS);
                assertEquals(
                        ServiceWireConstants.COMMAND_CHANNEL_SEND, receive(peerPort, peer, wire));
            }
            for (int period = 0; period < 2; period++) {
                tick(node, System.nanoTime() + Duration.ofSeconds(5L * period).toNanos());
                // A subsequent probe proves this management turn submitted no HELLO.
                int command;
                do {
                    command = receive(peerPort, peer, wire);
                    assertNotEquals(ServiceWireConstants.COMMAND_HELLO, command);
                } while (command != ServiceWireConstants.COMMAND_LIVENESS_PROBE);
            }
        }
    }

    private static final class PeerReadyEvents extends java.util.logging.Handler
            implements AutoCloseable {
        private final java.util.logging.Logger logger =
                java.util.logging.Logger.getLogger(ZLinkJavaRawMeshNode.class.getName());
        private final String message;
        private final AtomicLong count = new AtomicLong();

        PeerReadyEvents(RoutingId peer) {
            message = "ZLINK_FRAMEWORK_PEER_READY mesh=hello-retry peer=" + peer;
            logger.addHandler(this);
        }

        @Override
        public void publish(java.util.logging.LogRecord record) {
            if (message.equals(record.getMessage())) count.incrementAndGet();
        }

        @Override
        public void flush() {}

        @Override
        public void close() {
            logger.removeHandler(this);
        }
    }

    private static void assertNotReady(CompletionStage<?> result) {
        var failure =
                assertThrows(CompletionException.class, () -> result.toCompletableFuture().join());
        var cause = failure.getCause();
        if (cause instanceof ZlinkSubmitException submit) {
            assertEquals(SubmitResult.NOT_CONNECTED, submit.getResult());
        } else if (cause instanceof ZlinkRequestException request) {
            assertEquals(RequestResult.NOT_CONNECTED, request.getResult());
        } else {
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, cause).kind());
        }
    }

    private static int receive(
            ZLinkJavaRawServicePort port, RouterSocket router, ZLinkServiceM6AWireCodec wire) {
        return wire.decodeHeader(receiveFrame(port, router)).command();
    }

    private static byte[] receiveFrame(ZLinkJavaRawServicePort port, RouterSocket router) {
        while (true) {
            assertTrue(port.waitForReadable(router, IO_DEADLINE), "expected protocol record");
            try (var inbound = port.receive(router).orElseThrow()) {
                if (inbound.frames().getFirst().length == 0) continue;
                return inbound.frames().getFirst();
            }
        }
    }

    private static void dispatchNext(
            ZLinkJavaRawMeshNode node, ZLinkJavaRawServicePort port, RouterSocket router)
            throws Exception {
        while (true) {
            assertTrue(port.waitForReadable(router, IO_DEADLINE));
            var inbound = port.receive(router).orElseThrow();
            if (inbound.frames().getFirst().length == 0) {
                inbound.close();
                continue;
            }
            var dispatch =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "dispatch", ZLinkJavaRawServicePort.Inbound.class);
            dispatch.setAccessible(true);
            dispatch.invoke(node, inbound);
            return;
        }
    }

    private static void tick(ZLinkJavaRawMeshNode node, long now) throws Exception {
        var method = ZLinkJavaRawMeshNode.class.getDeclaredMethod("tickLiveness", long.class);
        method.setAccessible(true);
        method.invoke(node, now);
    }

    private static void invoke(Object owner, String name) throws Exception {
        var method = owner.getClass().getDeclaredMethod(name);
        method.setAccessible(true);
        method.invoke(owner);
    }

    private static Object field(Object owner, String name) throws Exception {
        var field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static void set(Object owner, String name, Object value) throws Exception {
        var field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(owner, value);
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
        return descriptor(rid, 1);
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid, long revision) {
        return new ZLinkServiceNodeDescriptor(
                "hello-retry",
                rid,
                1,
                revision,
                "inproc://" + rid,
                List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
                revision == 1
                        ? ZLinkServiceNodeDescriptor.State.PREPARING
                        : ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                1,
                1,
                100,
                0,
                0);
    }
}
