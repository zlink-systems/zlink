package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.UUID;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

final class ZLinkJavaRawMeshNodePumpTest {
    @Test
    void crossedHelloResponseKeepsItsSnapshotWhenAdmitIsClaimedInTheSameRotation()
            throws Exception {
        var localRid = RoutingId.from("crossed-local");
        var peerRid = RoutingId.from("crossed-peer");
        var codec = new ZLinkServiceM6AWireCodec();
        var localHello = admissionDescriptor(localRid, 1);
        var current = admissionDescriptor(localRid, 2);
        var remote = admissionDescriptor(peerRid, 1);
        try (var context = Zlink.createContext();
                var port = new ZLinkJavaRawServicePort(context);
                var peerPort = new ZLinkJavaRawServicePort(context);
                var router = port.openRouter(localRid);
                var peer = peerPort.openRouter(peerRid)) {
            String endpoint = "inproc://crossed-hello-" + UUID.randomUUID();
            peer.bind(endpoint);
            router.connect(endpoint);
            awaitRoute(port, router, peerRid);
            awaitRoute(peerPort, peer, localRid);
            var topology = new ZLinkServiceTopologyRegistry(current, port.stateLane());
            try (var node =
                    new ZLinkJavaRawMeshNode(
                            "rotation",
                            System::currentTimeMillis,
                            () -> 0L,
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(15),
                            port,
                            Runnable::run,
                            router,
                            current,
                            topology,
                            java.util.Map.of())) {
                for (int command :
                        new int[] {
                            ServiceWireConstants.COMMAND_HELLO, ServiceWireConstants.COMMAND_ADMIT
                        }) {
                    peerPort.send(peer, localRid, List.of(codec.encodeAdmission(command, remote)))
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                }
                var pending = new java.util.ArrayDeque<ZLinkJavaRawServicePort.Inbound>();
                try {
                    for (int index = 0; index < 2; index++) {
                        assertTrue(port.waitForReadable(router, Duration.ofSeconds(5)));
                        var inbound = port.receive(router);
                        assertNotNull(inbound);
                        pending.add(inbound);
                    }
                    node.helloExchanges.put(
                            peerRid,
                            new ZLinkJavaRawMeshNode.HelloExchange(
                                    peerRid,
                                    pending.getFirst().routeGeneration(),
                                    CompletableFuture.completedFuture(null),
                                    codec.encodeAdmission(
                                            ServiceWireConstants.COMMAND_HELLO, localHello)));
                    installClaims(
                            node,
                            () -> {
                                var inbound = pending.pollFirst();
                                if (inbound == null) return null;
                                return node.claimRecordForTest(inbound);
                            });
                    drain(node);
                    assertTrue(pending.isEmpty());
                    for (int command :
                            new int[] {
                                ServiceWireConstants.COMMAND_ADMIT,
                                ServiceWireConstants.COMMAND_UPDATE
                            }) {
                        assertTrue(peerPort.waitForReadable(peer, Duration.ofSeconds(5)));
                        try (var response = peerPort.receive(peer)) {
                            assertNotNull(response);
                            var head = response.frames().getFirst();
                            assertEquals(command, codec.decodeHeader(head).command());
                            assertEquals(
                                    command == ServiceWireConstants.COMMAND_ADMIT ? 1 : 2,
                                    codec.decodeAdmission(head, command, localRid)
                                            .descriptorRevision());
                        }
                    }
                } finally {
                    pending.forEach(ZLinkJavaRawServicePort.Inbound::close);
                }
            }
        }
    }

    private static void awaitRoute(
            ZLinkJavaRawServicePort port,
            systems.zlink.contracts.sockets.RouterSocket router,
            RoutingId peer) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (port.routesSnapshot(router).stream()
                .noneMatch(route -> route.routingId().equals(peer))) {
            long remaining = deadline - System.nanoTime();
            assertTrue(remaining > 0, "selected route was not observed");
            port.waitForReady(router, Duration.ofNanos(remaining));
        }
    }

    private static ZLinkServiceNodeDescriptor admissionDescriptor(RoutingId rid, long revision) {
        return new ZLinkServiceNodeDescriptor(
                "rotation",
                rid,
                1,
                revision,
                "inproc://" + rid,
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                1,
                1,
                0,
                0,
                0);
    }

    @Test
    void permitPressureTransitionsRunOutsideTheTransportTurn() throws Exception {
        try (var queue =
                        new ZLinkApplicationJobQueue(
                                ZLinkApplicationJobQueueProfile.BALANCED,
                                OptionalLong.of(4),
                                new ZLinkApplicationJobQueue.ProcessorCandidates(
                                        1, null, null, null),
                                100,
                                0);
                var context = Zlink.createContext();
                var node = rotationNode(context, () -> 0L)) {
            node.setApplicationJobQueue(queue);
            var transitions =
                    new java.util.ArrayList<systems.zlink.contracts.sockets.ReceiveFlowState>();
            try (var observation =
                    queue.registerReceiveFlowTarget(
                            state -> {
                                assertFalse(node.sourceTransportLane().isOnLane());
                                transitions.add(state);
                            })) {
                var claims = new AtomicInteger();
                var owned = new java.util.ArrayList<Received>();
                var pending = new java.util.ArrayDeque<Object>();
                for (int index = 0; index < 6; index++)
                    pending.add(admissionRecord(() -> {}, owned));
                installClaims(
                        node,
                        () -> {
                            var record = pending.pollFirst();
                            if (record != null) claims.incrementAndGet();
                            return record;
                        });
                drain(node);
                assertEquals(4, claims.get());
                assertEquals(2, pending.size());
                assertEquals(
                        List.of(
                                systems.zlink.contracts.sockets.ReceiveFlowState.RUNNING,
                                systems.zlink.contracts.sockets.ReceiveFlowState.PAUSED,
                                systems.zlink.contracts.sockets.ReceiveFlowState.RUNNING),
                        transitions);
                drain(node);
                assertEquals(6, claims.get());
                assertTrue(pending.isEmpty());
                for (var record : owned)
                    org.junit.jupiter.api.Assertions.assertThrows(
                            IllegalStateException.class, record::parts);
            }
        }
    }

    @Test
    void admissionPublicationsKeepReceiveOrderOutsideTheTransportLane() throws Exception {
        try (var context = Zlink.createContext();
                var node = rotationNode(context, () -> 0L)) {
            var order = new java.util.ArrayList<Integer>();
            var claims = new AtomicInteger();
            installClaims(
                    node,
                    () -> {
                        int index = claims.getAndIncrement();
                        if (index == 3) return null;
                        return admissionRecord(
                                () -> {
                                    assertFalse(node.sourceTransportLane().isOnLane());
                                    order.add(index);
                                },
                                new java.util.ArrayList<>());
                    });
            drain(node);
            assertEquals(List.of(0, 1, 2), order);
        }
    }

    @Test
    void claimAndDispatchFailuresReleaseEveryRecordAndPermit() throws Exception {
        for (boolean failClaim : new boolean[] {true, false}) {
            try (var queue =
                            new ZLinkApplicationJobQueue(
                                    ZLinkApplicationJobQueueProfile.BALANCED,
                                    OptionalLong.of(100),
                                    new ZLinkApplicationJobQueue.ProcessorCandidates(
                                            1, null, null, null),
                                    100,
                                    0);
                    var context = Zlink.createContext();
                    var node = rotationNode(context, () -> 0L)) {
                node.setApplicationJobQueue(queue);
                var received = new java.util.ArrayList<Received>();
                var claims = new AtomicInteger();
                var failure = new IllegalStateException("rotation failure");
                var published = new java.util.ArrayList<Integer>();
                installClaims(
                        node,
                        () -> {
                            int index = claims.getAndIncrement();
                            if (index == 3) {
                                if (failClaim) throw failure;
                                return null;
                            }
                            return admissionRecord(
                                    () -> {
                                        if (!failClaim && index == 1) throw failure;
                                        published.add(index);
                                    },
                                    received);
                        });
                drain(node);
                assertEquals(failClaim ? List.of(0, 1, 2) : List.of(0, 2), published);
                assertEquals(3, received.size());
                for (var record : received)
                    org.junit.jupiter.api.Assertions.assertThrows(
                            IllegalStateException.class, record::parts);
                int returned = queue.reserveRotationBudget(100);
                try {
                    assertEquals(100, returned);
                } finally {
                    queue.abandonRotationBudget(returned);
                }
            }
        }
    }

    @Test
    void malformedChannelSendSettlesIndependentlyAndPumpContinues() throws Exception {
        var drops = new LinkedBlockingQueue<Map<String, String>>();
        try (var metrics =
                        ZLinkRuntimeMetrics.install(
                                new ZLinkRuntimeMetrics.Sink() {
                                    @Override
                                    public void increment(String name, Map<String, String> tags) {
                                        if (name.equals("zlink.mesh_node.messages.dropped"))
                                            drops.add(tags);
                                    }
                                });
                var fixture = new RotationRequests()) {
            var a = fixture.request(1);
            byte[] malformed = fixture.codec.encodeChannelSendHeader("orders", 0);
            fixture.sourcePort
                    .send(
                            fixture.source,
                            fixture.targetRid,
                            List.of(java.util.Arrays.copyOf(malformed, malformed.length - 1)))
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var c = fixture.request(3);
            fixture.start();
            fixture.reply(1);
            assertEquals(3, fixture.claims.get(), "A, B and C must share the claim turn");
            fixture.reply(3);
            fixture.assertReply(a, 1);
            fixture.assertReply(c, 3);
            assertEquals(
                    Map.of(
                            "mesh_name",
                            "rotation",
                            "surface",
                            "channel",
                            "message_kind",
                            "send",
                            "reason",
                            "decode_error"),
                    drops.poll(5, TimeUnit.SECONDS));
            assertTrue(drops.isEmpty(), "B is classified once");
            var later = fixture.request(4);
            fixture.reply(4);
            fixture.assertReply(later, 4);
            fixture.assertPermitsReturned();
        }
    }

    @Test
    void malformedRequestHeaderDoesNotCountAsAOneWayFailure() throws Exception {
        var drops = new LinkedBlockingQueue<Map<String, String>>();
        try (var metrics =
                        ZLinkRuntimeMetrics.install(
                                new ZLinkRuntimeMetrics.Sink() {
                                    @Override
                                    public void increment(String name, Map<String, String> tags) {
                                        if (name.equals("zlink.mesh_node.messages.dropped"))
                                            drops.add(tags);
                                    }
                                });
                var fixture = new RotationRequests()) {
            byte[] malformed = fixture.codec.encodeNodeRequestHeader(2, 0);
            fixture.sourcePort
                    .send(
                            fixture.source,
                            fixture.targetRid,
                            List.of(java.util.Arrays.copyOf(malformed, malformed.length - 1)))
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var sibling = fixture.request(3);
            fixture.start();
            fixture.reply(3);
            fixture.assertReply(sibling, 3);
            assertTrue(
                    drops.isEmpty(),
                    "a request is not a one-way record when correlation decode fails");
            fixture.assertPermitsReturned();
        }
    }

    @Test
    void enqueueFailureAndCancellationKeepTheirClassificationAndSiblingReplies() throws Exception {
        for (RuntimeException failure :
                List.of(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE, "rejected"),
                        new CancellationException("cancelled"),
                        new IllegalStateException("enqueue failed"),
                        new IllegalArgumentException("enqueue validation failed"))) {
            try (var fixture = new RotationRequests()) {
                fixture.node.setApplicationReceiver(
                        receiver(
                                record -> {
                                    if (record.receive().applicationCorrelation() == 2L)
                                        throw failure;
                                    fixture.enqueue(record);
                                }));
                var a = fixture.request(1);
                var b = fixture.request(2);
                var c = fixture.request(3);
                fixture.start();
                fixture.reply(1);
                fixture.reply(3);
                fixture.assertReply(a, 1);
                fixture.assertReply(c, 3);
                var terminal =
                        fixture.codec.decodeReplyHeader(b.get(5, TimeUnit.SECONDS).getFirst());
                int[] expected = ZLinkRequestFailureMapping.outgoing(failure);
                assertEquals(2, terminal.correlation());
                assertEquals(expected[0], terminal.terminalResult());
                assertEquals(expected[1], terminal.failureCode());
                fixture.assertPermitsReturned();
            }
        }
    }

    @Test
    void fatalErrorStopsClaimOrDispatchAndReleasesOwnedResources() throws Exception {
        for (boolean failClaim : new boolean[] {true, false}) {
            try (var context = Zlink.createContext();
                    var node = rotationNode(context, () -> 0L)) {
                var received = new java.util.ArrayList<Received>();
                var claims = new AtomicInteger();
                var published = new java.util.ArrayList<Integer>();
                var failure = new AssertionError("fatal failure");
                installClaims(
                        node,
                        () -> {
                            int index = claims.getAndIncrement();
                            if (index == 3) {
                                if (failClaim) throw failure;
                                return null;
                            }
                            return admissionRecord(
                                    () -> {
                                        if (!failClaim && index == 1) throw failure;
                                        published.add(index);
                                    },
                                    received);
                        });
                org.junit.jupiter.api.Assertions.assertSame(
                        failure, assertThrows(AssertionError.class, () -> drain(node)));
                assertEquals(failClaim ? List.of() : List.of(0), published);
                for (var record : received)
                    assertThrows(IllegalStateException.class, record::parts);
            }
        }
    }

    private static final class RotationRequests implements AutoCloseable {
        final Context context = Zlink.createContext();
        final ZLinkJavaRawServicePort sourcePort = new ZLinkJavaRawServicePort(context);
        final ZLinkJavaRawServicePort targetPort = new ZLinkJavaRawServicePort(context);
        final RoutingId sourceRid = RoutingId.from("rotation-source");
        final RoutingId targetRid = RoutingId.from("rotation-target");
        final RouterSocket source = sourcePort.openRouter(sourceRid);
        final RouterSocket target = targetPort.openRouter(targetRid, true);
        final ZLinkServiceM6AWireCodec codec = new ZLinkServiceM6AWireCodec();
        final ZLinkApplicationJobQueue queue =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(100),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
                        100,
                        0);
        final LinkedBlockingQueue<ZLinkMeshDispatchRecord> accepted = new LinkedBlockingQueue<>();
        final List<ZLinkApplicationJobContext.QueuedOwnership> queued = new java.util.ArrayList<>();
        final AtomicInteger claims = new AtomicInteger();
        final ZLinkJavaRawMeshNode node;

        RotationRequests() {
            var descriptor = admissionDescriptor(targetRid, 1);
            node =
                    new ZLinkJavaRawMeshNode(
                            "rotation",
                            System::currentTimeMillis,
                            () -> 0L,
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(15),
                            targetPort,
                            Runnable::run,
                            target,
                            descriptor,
                            new ZLinkServiceTopologyRegistry(descriptor, targetPort.stateLane()),
                            Map.of());
            String endpoint = "inproc://rotation-requests-" + UUID.randomUUID();
            target.bind(endpoint);
            source.connect(endpoint);
            awaitRoute(sourcePort, source, targetRid);
            awaitRoute(targetPort, target, sourceRid);
            sourcePort.ensureReceivePollerRegistered(source);
            targetPort.ensureReceivePollerRegistered(target);
            node.setApplicationJobQueue(queue);
            node.setApplicationReceiver(receiver(this::enqueue));
        }

        void enqueue(ZLinkMeshDispatchRecord record) {
            // Keep QUEUED permits until the test enters each accepted application job.
            queued.add(ZLinkApplicationJobContext.transferToQueuedJob());
            accepted.add(record);
        }

        CompletableFuture<List<byte[]>> request(long correlation) throws Exception {
            var admitted = new CompletableFuture<Void>();
            CompletableFuture<List<byte[]>> result;
            try (var packet = Message.from("packet")) {
                result =
                        sourcePort
                                .request(
                                        source,
                                        targetRid,
                                        List.of(
                                                codec.encodeNodeRequestHeader(correlation, 0),
                                                ZLinkServiceM6AWireCodec
                                                        .encodeFrameworkMultipartFrame(
                                                                List.of(packet))),
                                        Duration.ofSeconds(10),
                                        admitted)
                                .toCompletableFuture();
            }
            sourcePort.waitForReadable(source, Duration.ofMillis(100));
            CompletableFuture.anyOf(admitted, result).get(5, TimeUnit.SECONDS);
            assertTrue(admitted.isDone(), "native request must be admitted");
            return result;
        }

        void install() {
            installClaims(
                    node,
                    () -> {
                        var inbound = targetPort.receiveNow(target);
                        if (inbound == null) return null;
                        claims.incrementAndGet();
                        byte[] head = inbound.frames().getFirst();
                        return ZLinkJavaRawMeshNode.claimedRecordForTest(
                                inbound, Optional.empty(), head, codec.decodeHeader(head), null);
                    });
        }

        void start() {
            install();
            node.pumpForTest(Executors.newSingleThreadExecutor());
            node.pumpForTest().execute(() -> node.pumpTurnForTest(target));
        }

        void reply(long correlation) throws Exception {
            var record = accepted.poll(5, TimeUnit.SECONDS);
            assertNotNull(record, "request " + correlation + " was not accepted");
            assertEquals(correlation, record.receive().applicationCorrelation());
            try (record;
                    var reply = Message.from("reply-" + correlation)) {
                record.reply(List.of(reply));
            }
        }

        void assertReply(CompletableFuture<List<byte[]>> result, long correlation)
                throws Exception {
            sourcePort.waitForReadable(source, Duration.ofMillis(100));
            var frames = result.get(5, TimeUnit.SECONDS);
            var reply = codec.decodeReplyHeader(frames.getFirst());
            assertEquals(correlation, reply.correlation());
            assertEquals(0, reply.terminalResult());
            assertEquals(0, reply.failureCode());
            var parts =
                    ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrame(
                            java.nio.ByteBuffer.wrap(frames.get(1)));
            try {
                assertEquals("reply-" + correlation, parts.getFirst().toUtf8String());
            } finally {
                Message.closeAll(parts);
            }
        }

        void assertPermitsReturned() {
            // Stop the receive owner before checking RESERVED and QUEUED ownership.
            node.close();
            int reserved = queue.reserveRotationBudget(100);
            queue.abandonRotationBudget(reserved);
            assertEquals(100 - queued.size(), reserved, "rotation must keep QUEUED permits");
            queued.forEach(ZLinkApplicationJobContext.QueuedOwnership::close);
            int returned = queue.reserveRotationBudget(100);
            queue.abandonRotationBudget(returned);
            assertEquals(100, returned);
        }

        @Override
        public void close() {
            node.close();
            accepted.forEach(ZLinkMeshDispatchRecord::close);
            queued.forEach(ZLinkApplicationJobContext.QueuedOwnership::close);
            sourcePort.close();
            targetPort.close();
            queue.close();
            context.close();
        }
    }

    @Test
    void expiredRotationBudgetStillAllowsItsFirstClaim() throws Exception {
        var reads = new AtomicInteger();
        try (var context = Zlink.createContext();
                var node =
                        rotationNode(
                                context,
                                () ->
                                        reads.getAndIncrement() == 0
                                                ? 0
                                                : TimeUnit.MILLISECONDS.toNanos(3))) {
            reads.set(0);
            var claims = new AtomicInteger();
            var received = new java.util.ArrayList<Received>();
            installClaims(
                    node,
                    () -> {
                        claims.incrementAndGet();
                        return admissionRecord(() -> {}, received);
                    });
            drain(node);
            assertEquals(1, claims.get());
            assertEquals(1, received.size());
        }
    }

    @Test
    void closeDuringPublicationStopsRotationAndEveryRequestReachesATerminal() throws Exception {
        try (var fixture = new RotationRequests()) {
            var requests = List.of(fixture.request(1), fixture.request(2), fixture.request(3));
            var publications = new AtomicInteger();
            fixture.node.setApplicationReceiver(
                    receiver(
                            record -> {
                                publications.incrementAndGet();
                                try (record;
                                        var reply = Message.from("reply")) {
                                    record.reply(List.of(reply));
                                }
                                fixture.node.close();
                            }));
            fixture.install();
            drain(fixture.node);
            assertEquals(3, fixture.claims.get());
            assertEquals(1, publications.get());
            for (var request : requests) {
                fixture.sourcePort.waitForReadable(fixture.source, Duration.ofMillis(100));
                try {
                    request.get(2, TimeUnit.SECONDS);
                } catch (ExecutionException terminal) {
                    var failure =
                            org.junit.jupiter.api.Assertions.assertInstanceOf(
                                    ZLinkFrameworkException.class, terminal.getCause());
                    assertFalse(
                            failure.kind() == ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            "shutdown must not leave an accepted request waiting for its deadline");
                    var result = ZLinkJavaRawMeshNode.requestResult(failure.getCause());
                    assertNotNull(result, "shutdown must preserve a typed binding terminal");
                    assertFalse(result == RequestResult.TIMED_OUT);
                }
                assertTrue(request.isDone());
            }
            fixture.assertPermitsReturned();
        }
    }

    private static ZLinkMeshApplicationReceiver receiver(Consumer<ZLinkMeshDispatchRecord> accept) {
        return new ZLinkMeshApplicationReceiver() {
            @Override
            public void accept(ZLinkMeshDispatchRecord record) {
                accept.accept(record);
            }

            @Override
            public CompletionStage<Integer> submitLocalNodeSend(
                    RoutingId rid, byte[] metadata, List<Message> parts) {
                throw new AssertionError("remote request fixture");
            }
        };
    }

    private static ZLinkJavaRawMeshNode rotationNode(
            Context context, java.util.function.LongSupplier clock) {
        return new ZLinkJavaRawMeshNode(
                context,
                "rotation",
                System::currentTimeMillis,
                clock,
                Duration.ofSeconds(5),
                Duration.ofSeconds(15));
    }

    private static Object admissionRecord(Runnable publication, List<Received> owned) {
        var received =
                new Received(
                        RoutingId.from("rotation-source"),
                        new Message[] {Message.from(new byte[] {1})});
        owned.add(received);
        var inbound =
                new ZLinkJavaRawServicePort.Inbound(
                        RoutingId.from("rotation-source"), null, received);
        return ZLinkJavaRawMeshNode.claimedRecordForTest(
                inbound,
                java.util.Optional.empty(),
                new byte[] {1},
                new ZLinkServiceM6AWireCodec.Header(ServiceWireConstants.COMMAND_HELLO, 0),
                publication);
    }

    private static void installClaims(
            ZLinkJavaRawMeshNode node, java.util.function.Supplier<?> claims) {
        node.pumpClaimForTest(claims);
    }

    private static void drain(ZLinkJavaRawMeshNode node) {
        node.drainIngressBatchForTest(null);
    }

    @Test
    void oneReceiveRotationClaimsItsBudgetBeforeTheNextStateLaneTurn() throws Exception {
        try (var context = Zlink.createContext();
                var node =
                        new ZLinkJavaRawMeshNode(
                                context,
                                "claim-turn",
                                System::currentTimeMillis,
                                () -> 0L,
                                Duration.ofSeconds(5),
                                Duration.ofSeconds(15));
                var worker = Executors.newVirtualThreadPerTaskExecutor()) {
            var lane = node.sourceTransportLane();
            var claims = new AtomicInteger();
            var claimsBeforeNextTurn = new CompletableFuture<Integer>();
            installClaims(
                    node,
                    (java.util.function.Supplier<?>)
                            () -> {
                                assertTrue(lane.isOnLane());
                                int count = claims.incrementAndGet();
                                if (count == 1) {
                                    CompletableFuture.runAsync(
                                                    () ->
                                                            lane.runAsync(
                                                                    () -> {
                                                                        claimsBeforeNextTurn
                                                                                .complete(
                                                                                        claims
                                                                                                .get());
                                                                        return null;
                                                                    }),
                                                    worker)
                                            .orTimeout(5, TimeUnit.SECONDS)
                                            .join();
                                }
                                if (count > ZLinkJavaRawMeshNode.ingressBatchLimit()) return null;
                                var received =
                                        new Received(
                                                RoutingId.from("claim-source"),
                                                new Message[] {Message.from(new byte[] {1})});
                                var inbound =
                                        new ZLinkJavaRawServicePort.Inbound(
                                                RoutingId.from("claim-source"), null, received);
                                return node.claimRecordForTest(inbound);
                            });
            node.drainIngressBatchForTest(null);
            assertEquals(ZLinkJavaRawMeshNode.ingressBatchLimit(), claims.get());
            assertEquals(
                    ZLinkJavaRawMeshNode.ingressBatchLimit(),
                    claimsBeforeNextTurn.get(5, TimeUnit.SECONDS));
            assertFalse(lane.isOnLane());
        }
    }

    @Test
    void serialRequestsKeepTheReceiveTaskUntilShutdown() throws Exception {
        runSerialRequests(false);
    }

    @Test
    void pendingPermitYieldsToItsGrantOnTheSameExecutor() throws Exception {
        runSerialRequests(true);
    }

    private static void runSerialRequests(boolean exhaustPermit) throws Exception {
        CountingExecutor sourcePump;
        CountingExecutor targetPump;
        try (var queue =
                        new ZLinkApplicationJobQueue(
                                ZLinkApplicationJobQueueProfile.BALANCED,
                                OptionalLong.of(1),
                                new ZLinkApplicationJobQueue.ProcessorCandidates(
                                        1, null, null, null),
                                100,
                                0);
                var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "pump");
                var target = new ZLinkJavaRawMeshNode(context, "pump")) {
            RoutingId sourceRid = RoutingId.from("pump-source");
            RoutingId targetRid = RoutingId.from("pump-target");
            String targetEndpoint = "inproc://pump-target-" + UUID.randomUUID();
            var incoming = new LinkedBlockingQueue<ZLinkMeshDispatchRecord>();
            target.setApplicationReceiver(
                    new systems.zlink.framework.runtime.internal.backend
                            .ZLinkMeshApplicationReceiver() {
                        @Override
                        public void accept(ZLinkMeshDispatchRecord record) {
                            incoming.add(record);
                        }

                        @Override
                        public CompletionStage<Integer> submitLocalNodeSend(
                                RoutingId sourceNodeRid, byte[] metadata, List<Message> parts) {
                            throw new AssertionError("remote request fixture");
                        }
                    });
            source.setRoutingId(sourceRid);
            target.setRoutingId(targetRid);
            source.setBind("inproc://pump-source-" + UUID.randomUUID());
            target.setBind(targetEndpoint);
            source.start();
            target.start();
            sourcePump = countPump(source);
            targetPump = countPump(target);
            var ready = new CompletableFuture<Void>();
            try (var observation =
                    source.onPeerStateChanged(
                            targetRid,
                            () -> {
                                if (source.observePeerRoute(targetRid).ready())
                                    ready.complete(null);
                            },
                            () -> {})) {
                source.connectPeer(targetEndpoint, targetRid);
                ready.get(5, TimeUnit.SECONDS);
            }
            // Install after startup to isolate permit callbacks from Core PAUSED flow control.
            // This white-box case exercises the receive executor's grant queue directly.
            ZLinkApplicationJobQueue.Permit held = null;
            if (exhaustPermit) {
                target.setApplicationJobQueue(queue);
                held =
                        queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                                .toCompletableFuture()
                                .join();
            }
            try {
                for (int i = 0; i < 3; i++) {
                    try (var packet = Message.from("pump.packet");
                            var payload = Message.from("request")) {
                        var reply =
                                source.requestNode(
                                        targetRid,
                                        null,
                                        List.of(packet, payload),
                                        Duration.ofSeconds(5),
                                        null,
                                        null,
                                        null);
                        if (held != null) {
                            var pending = target.pendingReceiveAcquireForTest();
                            var yielded = new CompletableFuture<Boolean>();
                            targetPump.execute(() -> yielded.complete(pending.get() != null));
                            assertTrue(yielded.get(5, TimeUnit.SECONDS));
                            assertTrue(incoming.isEmpty());
                            assertFalse(reply.toCompletableFuture().isDone());
                            held.close();
                            held = null;
                        }
                        var record = incoming.poll(5, TimeUnit.SECONDS);
                        assertNotNull(record);
                        try (record;
                                var response = Message.from("reply")) {
                            record.reply(List.of(response));
                        }
                        try (var received = reply.toCompletableFuture().get(5, TimeUnit.SECONDS)) {
                            assertEquals("reply", received.parts().getFirst().toUtf8String());
                        }
                    }
                }
            } finally {
                if (held != null) held.close();
            }
        }
        assertTrue(sourcePump.isTerminated());
        assertTrue(targetPump.isTerminated());
        assertEquals(0, sourcePump.posts.get(), "source pump reposted a receive rotation");
        if (!exhaustPermit)
            assertEquals(0, targetPump.posts.get(), "target pump reposted a receive rotation");
        else
            // The probe above is one post; the grant and the resumed rotation must add more.
            assertTrue(
                    targetPump.posts.get() >= 2, "pending grant must run on the receive executor");
    }

    private static CountingExecutor countPump(ZLinkJavaRawMeshNode node) {
        var counting = new CountingExecutor(node.pumpForTest());
        node.pumpForTest(counting);
        return counting;
    }

    private static final class CountingExecutor extends AbstractExecutorService {
        private final ExecutorService delegate;
        private final AtomicInteger posts = new AtomicInteger();

        private CountingExecutor(ExecutorService delegate) {
            this.delegate = delegate;
        }

        @Override
        public void execute(Runnable command) {
            posts.incrementAndGet();
            delegate.execute(command);
        }

        @Override
        public void shutdown() {
            delegate.shutdown();
        }

        @Override
        public List<Runnable> shutdownNow() {
            return delegate.shutdownNow();
        }

        @Override
        public boolean isShutdown() {
            return delegate.isShutdown();
        }

        @Override
        public boolean isTerminated() {
            return delegate.isTerminated();
        }

        @Override
        public boolean awaitTermination(long timeout, TimeUnit unit) throws InterruptedException {
            return delegate.awaitTermination(timeout, unit);
        }
    }
}
