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
    void selectedHelloBeforeSnapshotReadiesBothPeersOnFirstExchange() throws Exception {
        var localRid = RoutingId.from("snapshot-local");
        var remoteRid = RoutingId.from("snapshot-remote");
        var wire = new ZLinkServiceM6AWireCodec();
        try (var context = Zlink.createContext()) {
            var localPort = new ZLinkJavaRawServicePort(context);
            var remotePort = new ZLinkJavaRawServicePort(context);
            var localRouter = localPort.openRouter(localRid);
            var remoteRouter = remotePort.openRouter(remoteRid);
            var localDescriptor = descriptor(localRid, 2);
            var remoteDescriptor = descriptor(remoteRid, 2);
            var mutations = new AtomicLong();
            var offOwnerMutations = new AtomicLong();
            var readiness =
                    new java.util.HashMap<RoutingId, String>() {
                        @Override
                        public String put(RoutingId rid, String connection) {
                            mutations.incrementAndGet();
                            if (!localPort.stateLane().isOnLane())
                                offOwnerMutations.incrementAndGet();
                            return super.put(rid, connection);
                        }
                    };
            var localTopology =
                    systems.zlink.framework.runtime.internal.service
                            .ZLinkServiceTopologyRegistryTesting.create(
                            localDescriptor, localPort.stateLane(), readiness);
            var remoteTopology =
                    new ZLinkServiceTopologyRegistry(remoteDescriptor, remotePort.stateLane());
            try (var local =
                            ZLinkJavaRawMeshNodeTesting.create(
                                    "hello-retry",
                                    localPort,
                                    localRouter,
                                    localDescriptor,
                                    localTopology,
                                    java.util.Map.of(),
                                    Runnable::run);
                    var remote =
                            ZLinkJavaRawMeshNodeTesting.create(
                                    "hello-retry",
                                    remotePort,
                                    remoteRouter,
                                    remoteDescriptor,
                                    remoteTopology,
                                    java.util.Map.of(),
                                    Runnable::run);
                    var events =
                            new PeerReadyEvents(
                                    remoteRid,
                                    localPort.stateLane()::isOnLane,
                                    local::readyPeerCount)) {
                var endpoint = "inproc://snapshot-" + java.util.UUID.randomUUID();
                localRouter.options().probe(true);
                remoteRouter.options().probe(true);
                localRouter.bind(endpoint);
                remoteRouter.connect(endpoint);
                localPort.ensureReceivePollerRegistered(localRouter);
                remotePort.ensureReceivePollerRegistered(remoteRouter);
                long deadline = System.nanoTime() + IO_DEADLINE.toNanos();
                while (localPort.routesSnapshot(localRouter).isEmpty()
                        || remotePort.routesSnapshot(remoteRouter).isEmpty()) {
                    assertTrue(System.nanoTime() < deadline, "Core must select both routes");
                    Thread.onSpinWait();
                }
                // No pump/tick/framework route observation precedes these first HELLOs.
                localPort
                        .send(
                                localRouter,
                                remoteRid,
                                List.of(
                                        wire.encodeAdmission(
                                                ServiceWireConstants.COMMAND_HELLO,
                                                localDescriptor)))
                        .toCompletableFuture()
                        .join();
                remotePort
                        .send(
                                remoteRouter,
                                localRid,
                                List.of(
                                        wire.encodeAdmission(
                                                ServiceWireConstants.COMMAND_HELLO,
                                                remoteDescriptor)))
                        .toCompletableFuture()
                        .join();
                long delivered =
                        dispatchNext(
                                local, localPort, localRouter, ServiceWireConstants.COMMAND_HELLO);
                assertNotEquals(0L, delivered);
                assertEquals(
                        ZLinkJavaRawMeshNode.routeConnectionId(delivered),
                        localTopology.peer(remoteRid).orElseThrow().connectionId());
                dispatchNext(remote, remotePort, remoteRouter, ServiceWireConstants.COMMAND_HELLO);
                dispatchNext(remote, remotePort, remoteRouter, ServiceWireConstants.COMMAND_ADMIT);
                dispatchNext(local, localPort, localRouter, ServiceWireConstants.COMMAND_ADMIT);
                assertTrue(localTopology.isReady(remoteRid));
                assertTrue(remoteTopology.isReady(localRid));
                assertEquals(1, local.readyPeerCount());
                assertEquals(1, remote.readyPeerCount());
                assertTrue(local.observePeerRoute(remoteRid).ready());
                assertTrue(remote.observePeerRoute(localRid).ready());
                assertTrue(mutations.get() > 0, "actual admission mutates the injected source map");
                assertEquals(0, offOwnerMutations.get());
                assertEquals(1, events.count.get());
                assertEquals(0, events.ownerCallbacks.get());
                assertEquals(
                        1,
                        events.observedReadyCount.get(),
                        "READY handler reenters the final root");

                // Finish the original UPDATEs before the next expected command in each FIFO.
                dispatchNext(local, localPort, localRouter, ServiceWireConstants.COMMAND_UPDATE);
                assertEquals(
                        ServiceWireConstants.COMMAND_UPDATE,
                        receive(remotePort, remoteRouter, wire));
                local.observePeerAdmissionExpectation(
                                remoteRid,
                                remoteDescriptor.advertisedEndpoint(),
                                remoteDescriptor.lifecycleGeneration(),
                                "mismatched-security",
                                systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.SERVER)
                        .toCompletableFuture()
                        .get(2, java.util.concurrent.TimeUnit.SECONDS);
                remotePort
                        .send(
                                remoteRouter,
                                localRid,
                                List.of(
                                        wire.encodeAdmission(
                                                ServiceWireConstants.COMMAND_HELLO,
                                                remoteDescriptor)))
                        .toCompletableFuture()
                        .get(2, java.util.concurrent.TimeUnit.SECONDS);
                dispatchNext(local, localPort, localRouter, ServiceWireConstants.COMMAND_HELLO);
                assertEquals(
                        ServiceWireConstants.COMMAND_REJECT,
                        receive(remotePort, remoteRouter, wire));
                assertEquals(1, events.warnings.get());
                assertEquals(
                        2,
                        events.observedCallbacks.get(),
                        "both synchronous log callbacks were observed");
                assertEquals(
                        0,
                        events.ownerCallbacks.get(),
                        "warning and READY handlers run after the owner commit");
                assertEquals(
                        1,
                        events.observedReadyCount.get(),
                        "warning handler can reenter the published root");
                assertEquals(0, offOwnerMutations.get());
            }
        }
    }

    @Test
    void invalidAdmissionPayloadStillRejectsAfterTheHeaderClaimCloses() throws Exception {
        var localRid = RoutingId.from("invalid-local");
        var remoteRid = RoutingId.from("invalid-remote");
        var wire = new ZLinkServiceM6AWireCodec();
        try (var context = Zlink.createContext();
                var remotePort = new ZLinkJavaRawServicePort(context)) {
            var localPort = new ZLinkJavaRawServicePort(context);
            var localRouter = localPort.openRouter(localRid, true);
            var localDescriptor = descriptor(localRid, 2);
            var topology = new ZLinkServiceTopologyRegistry(localDescriptor, localPort.stateLane());
            try (var local =
                    ZLinkJavaRawMeshNodeTesting.create(
                            "hello-retry",
                            localPort,
                            localRouter,
                            localDescriptor,
                            topology,
                            java.util.Map.of(),
                            Runnable::run)) {
                var remoteRouter = remotePort.openRouter(remoteRid);
                remoteRouter.options().probe(true);
                String endpoint = "inproc://invalid-admission-" + java.util.UUID.randomUUID();
                localRouter.bind(endpoint);
                remoteRouter.connect(endpoint);
                var invalid =
                        new systems.zlink.framework.runtime.internal.service.ZLinkServiceWireCodec()
                                .encode(
                                        new systems.zlink.framework.runtime.internal.service
                                                .ZLinkServiceWireFrame(
                                                ServiceWireConstants.COMMAND_ADMIT, 0, List.of()))
                                .getFirst();
                remotePort
                        .send(remoteRouter, localRid, List.of(invalid))
                        .toCompletableFuture()
                        .get(2, java.util.concurrent.TimeUnit.SECONDS);
                dispatchNext(local, localPort, localRouter, ServiceWireConstants.COMMAND_ADMIT);
                assertTrue(topology.peers().isEmpty());
                assertTrue(local.peers().isEmpty());
                assertEquals(
                        ServiceWireConstants.COMMAND_REJECT,
                        wire.decodeHeader(receiveFrame(remotePort, remoteRouter)).command());
            }
        }
    }

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
            set(
                    node,
                    "topology",
                    new ZLinkServiceTopologyRegistry(descriptor(localRid), port.stateLane()));
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
            set(
                    node,
                    "topology",
                    new ZLinkServiceTopologyRegistry(descriptor(localRid), port.stateLane()));
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
        private final AtomicLong warnings = new AtomicLong();
        private final AtomicLong ownerCallbacks = new AtomicLong();
        private final AtomicLong observedCallbacks = new AtomicLong();
        private final AtomicLong observedReadyCount = new AtomicLong();
        private final String warningSuffix;
        private final java.util.function.BooleanSupplier isOnLane;
        private final java.util.function.LongSupplier readyPeerCount;

        PeerReadyEvents(RoutingId peer) {
            this(peer, null, null);
        }

        PeerReadyEvents(
                RoutingId peer,
                java.util.function.BooleanSupplier isOnLane,
                java.util.function.LongSupplier readyPeerCount) {
            message = "ZLINK_FRAMEWORK_PEER_READY mesh=hello-retry peer=" + peer;
            warningSuffix = " advertisedEndpoint=" + descriptor(peer, 2).advertisedEndpoint();
            this.isOnLane = isOnLane;
            this.readyPeerCount = readyPeerCount;
            logger.addHandler(this);
        }

        @Override
        public void publish(java.util.logging.LogRecord record) {
            boolean ready = message.equals(record.getMessage());
            boolean warning =
                    record.getMessage().contains("expected-route-mismatch mismatchFields=")
                            && record.getMessage().endsWith(warningSuffix);
            if (ready) count.incrementAndGet();
            if (warning) warnings.incrementAndGet();
            if ((ready || warning) && isOnLane != null) {
                observedCallbacks.incrementAndGet();
                if (isOnLane.getAsBoolean()) ownerCallbacks.incrementAndGet();
                observedReadyCount.set(readyPeerCount.getAsLong());
            }
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
            try (var inbound = port.receive(router)) {
                assertNotNull(inbound);
                if (inbound.frames().getFirst().length == 0) continue;
                return inbound.frames().getFirst();
            }
        }
    }

    private static long dispatchNext(
            ZLinkJavaRawMeshNode node, ZLinkJavaRawServicePort port, RouterSocket router)
            throws Exception {
        return dispatchNext(node, port, router, -1);
    }

    private static long dispatchNext(
            ZLinkJavaRawMeshNode node,
            ZLinkJavaRawServicePort port,
            RouterSocket router,
            int expectedCommand)
            throws Exception {
        long deadline = System.nanoTime() + IO_DEADLINE.toNanos();
        while (true) {
            assertTrue(System.nanoTime() < deadline, "expected admission record before deadline");
            assertTrue(port.waitForReadable(router, IO_DEADLINE));
            var inbound = port.receive(router);
            assertNotNull(inbound);
            if (inbound.frames().getFirst().length == 0) {
                inbound.close();
                continue;
            }
            long generation = inbound.routeGeneration();
            if (expectedCommand >= 0) {
                try {
                    assertEquals(
                            expectedCommand,
                            new ZLinkServiceM6AWireCodec()
                                    .decodeHeader(inbound.frames().getFirst())
                                    .command());
                } catch (Throwable failure) {
                    inbound.close();
                    throw failure;
                }
            }
            node.dispatch(inbound);
            return generation;
        }
    }

    private static void tick(ZLinkJavaRawMeshNode node, long now) {
        node.tickLiveness(now);
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
