package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.ContextOptions;
import systems.zlink.contracts.core.CoreHwmBudgetSnapshot;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ConnectResult;
import systems.zlink.contracts.errors.ZlinkConnectException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.sockets.ConnectionAttempt;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterRoute;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.RouterSocketOptions;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.contracts.sockets.XSubSocket;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.List;
import java.util.Map;
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
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

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
    void managementDisconnectFailureKeepsClaimedRequestsAndPumpAlive() throws Exception {
        var failure = new ZlinkConnectException(ConnectResult.INTERNAL_ERROR, 156);
        try (var reports = new ManagementReports();
                var fixture = new RotationRequests(failure)) {
            var expiredRid = RoutingId.from("rotation-expired");
            fixture.topology.admit(
                    admissionDescriptor(expiredRid, 1), "1", ServiceWireConstants.COMMAND_ADMIT);
            fixture.node.livenessForTest().admit(expiredRid, "1", 0L);
            fixture.now.set(TimeUnit.SECONDS.toNanos(16));
            var first = fixture.request(1);
            fixture.start();
            fixture.reply(1);
            fixture.assertReply(first, 1);
            org.junit.jupiter.api.Assertions.assertSame(
                    failure, reports.failures.poll(5, TimeUnit.SECONDS));
            var later = fixture.request(4);
            fixture.reply(4);
            fixture.assertReply(later, 4);
            fixture.assertPermitsReturned();
        }
    }

    @Test
    void idleManagementDisconnectFailureKeepsPumpAlive() throws Exception {
        var failure = new ZlinkConnectException(ConnectResult.INTERNAL_ERROR, 156);
        try (var reports = new ManagementReports();
                var fixture = new RotationRequests(failure, false)) {
            var expiredRid = RoutingId.from("rotation-expired");
            fixture.topology.admit(
                    admissionDescriptor(expiredRid, 1), "1", ServiceWireConstants.COMMAND_ADMIT);
            fixture.node.livenessForTest().admit(expiredRid, "1", 0L);
            fixture.now.set(TimeUnit.SECONDS.toNanos(16));
            fixture.start();
            org.junit.jupiter.api.Assertions.assertSame(
                    failure, reports.failures.poll(5, TimeUnit.SECONDS));
            assertEquals(0, fixture.claims.get(), "failure must occur on the idle management path");
            var later = fixture.request(4);
            fixture.reply(4);
            fixture.assertReply(later, 4);
            fixture.assertPermitsReturned();
        }
    }

    @Test
    void managementCreationFailureKeepsClaimedRequestsAndPumpAlive() throws Exception {
        var failure = new IllegalStateException("management clock failed");
        try (var reports = new ManagementReports();
                var fixture = new RotationRequests()) {
            var first = fixture.request(1);
            fixture.managementFailure.set(failure);
            fixture.start();
            fixture.reply(1);
            fixture.assertReply(first, 1);
            org.junit.jupiter.api.Assertions.assertSame(
                    failure, reports.failures.poll(5, TimeUnit.SECONDS));
            var later = fixture.request(4);
            fixture.reply(4);
            fixture.assertReply(later, 4);
            fixture.assertPermitsReturned();
        }
    }

    @ParameterizedTest
    @ValueSource(
            ints = {
                ServiceWireConstants.COMMAND_NODE_REQUEST,
                ServiceWireConstants.COMMAND_CHANNEL_REQUEST,
                ServiceWireConstants.COMMAND_SPOT_REQUEST,
                ServiceWireConstants.COMMAND_ACTOR_REQUEST
            })
    void inboundClaimFailureKeepsItsClassificationAndSiblingOrder(int command) throws Exception {
        try (var fixture = new RotationRequests()) {
            var a = fixture.request(1);
            var b = fixture.request(2, command);
            var c = fixture.request(3);
            fixture.failClaimCorrelation = 2L;
            fixture.start();
            fixture.reply(1);
            fixture.reply(3);
            assertEquals(3, fixture.claims.get());
            assertEquals(
                    List.of(0, 0, 0),
                    fixture.claimTurns,
                    "A, B and C must be claimed before the next state-lane turn");
            fixture.assertReply(a, 1);
            fixture.assertReply(c, 3);
            var terminal = fixture.codec.decodeReplyHeader(b.get(5, TimeUnit.SECONDS).getFirst());
            int[] expected = ZLinkRequestFailureMapping.outgoing(new NumberFormatException());
            assertEquals(2, terminal.correlation());
            assertEquals(expected[0], terminal.terminalResult());
            assertEquals(expected[1], terminal.failureCode());
            assertNotNull(fixture.failedInbound);
            assertThrows(IllegalStateException.class, fixture.failedInbound.received()::parts);
            var later = fixture.request(4);
            fixture.reply(4);
            fixture.assertReply(later, 4);
            fixture.assertPermitsReturned();
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

    private static final class ManagementReports implements AutoCloseable {
        final LinkedBlockingQueue<Throwable> failures = new LinkedBlockingQueue<>();
        final Logger logger = Logger.getLogger(ZLinkJavaRawMeshNode.class.getName());
        final Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        if (record.getThrown() != null) failures.add(record.getThrown());
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };

        ManagementReports() {
            logger.addHandler(handler);
        }

        @Override
        public void close() {
            logger.removeHandler(handler);
        }
    }

    private static final class DisconnectFailureRouter implements RouterSocket {
        private final RouterSocket nativeSocket;
        private final RuntimeException failure;
        private final AtomicInteger claims;
        private final boolean requiresClaim;

        DisconnectFailureRouter(
                RouterSocket nativeSocket,
                RuntimeException failure,
                AtomicInteger claims,
                boolean requiresClaim) {
            this.nativeSocket = nativeSocket;
            this.failure = failure;
            this.claims = claims;
            this.requiresClaim = requiresClaim;
        }

        @Override
        public RequestOperation request(RoutingId rid) {
            return nativeSocket.request(rid);
        }

        @Override
        public void bind(String v) {
            nativeSocket.bind(v);
        }

        @Override
        public void connect(String v) {
            nativeSocket.connect(v);
        }

        @Override
        public List<ConnectionAttempt> connectionAttemptsSnapshot() {
            return nativeSocket.connectionAttemptsSnapshot();
        }

        @Override
        public void unbind(String v) {
            nativeSocket.unbind(v);
        }

        @Override
        public void disconnect(String v) {
            nativeSocket.disconnect(v);
        }

        @Override
        public void disconnectRid(RoutingId v) {
            if (requiresClaim)
                assertTrue(
                        claims.get() > 0,
                        "management failure follows a healthy claim in the same rotation");
            throw failure;
        }

        @Override
        public void setRoutingId(RoutingId v) {
            nativeSocket.setRoutingId(v);
        }

        @Override
        public RoutingId getRoutingId() {
            return nativeSocket.getRoutingId();
        }

        @Override
        public List<RouterRoute> routesSnapshot() {
            return nativeSocket.routesSnapshot();
        }

        @Override
        public SendOperation send(RoutingId v) {
            return nativeSocket.send(v);
        }

        @Override
        public boolean recv(Received v, RecvFlags f) {
            return nativeSocket.recv(v, f);
        }

        @Override
        public ReplyOperation reply(RoutingId v, ReplyToken t) {
            return nativeSocket.reply(v, t);
        }

        @Override
        public RouterSocketOptions options() {
            return nativeSocket.options();
        }

        @Override
        public SocketMonitor monitorOpen() {
            return nativeSocket.monitorOpen();
        }

        @Override
        public SocketMonitor monitorOpen(MonitorEventType... e) {
            return nativeSocket.monitorOpen(e);
        }

        @Override
        public SocketMonitor monitorOpen(long h, MonitorEventType... e) {
            return nativeSocket.monitorOpen(h, e);
        }

        @Override
        public void setTlsServer(String c, String k, boolean r) {
            nativeSocket.setTlsServer(c, k, r);
        }

        @Override
        public void setTlsClient(String c, String h, boolean t) {
            nativeSocket.setTlsClient(c, h, t);
        }

        @Override
        public void close() {
            nativeSocket.close();
        }
    }

    private static final class RouterContext implements Context {
        private final Context nativeContext;
        private final RuntimeException failure;
        private final AtomicInteger claims;
        private final boolean requiresClaim;

        RouterContext(
                Context nativeContext,
                RuntimeException failure,
                AtomicInteger claims,
                boolean requiresClaim) {
            this.nativeContext = nativeContext;
            this.failure = failure;
            this.claims = claims;
            this.requiresClaim = requiresClaim;
        }

        @Override
        public RouterSocket createRouterSocket() {
            var router = nativeContext.createRouterSocket();
            return failure == null
                    ? router
                    : new DisconnectFailureRouter(router, failure, claims, requiresClaim);
        }

        @Override
        public ContextOptions options() {
            return nativeContext.options();
        }

        @Override
        public PairSocket createPairSocket() {
            return nativeContext.createPairSocket();
        }

        @Override
        public DealerSocket createDealerSocket() {
            return nativeContext.createDealerSocket();
        }

        @Override
        public PubSocket createPubSocket() {
            return nativeContext.createPubSocket();
        }

        @Override
        public SubSocket createSubSocket() {
            return nativeContext.createSubSocket();
        }

        @Override
        public XPubSocket createXPubSocket() {
            return nativeContext.createXPubSocket();
        }

        @Override
        public XSubSocket createXSubSocket() {
            return nativeContext.createXSubSocket();
        }

        @Override
        public StreamSocket createStreamSocket() {
            return nativeContext.createStreamSocket();
        }

        @Override
        public void shutdown() {
            nativeContext.shutdown();
        }

        @Override
        public void recalculateAutoHwm() {
            nativeContext.recalculateAutoHwm();
        }

        @Override
        public CoreHwmBudgetSnapshot coreHwmBudgetSnapshot() {
            return nativeContext.coreHwmBudgetSnapshot();
        }

        @Override
        public void resetCoreHwmBudgetMetrics() {
            nativeContext.resetCoreHwmBudgetMetrics();
        }

        @Override
        public void close() {
            nativeContext.close();
        }
    }

    private static final class RotationRequests implements AutoCloseable {
        final Context context = Zlink.createContext();
        final ZLinkJavaRawServicePort sourcePort = new ZLinkJavaRawServicePort(context);
        final ZLinkJavaRawServicePort targetPort;
        final RoutingId sourceRid = RoutingId.from("rotation-source");
        final RoutingId targetRid = RoutingId.from("rotation-target");
        final RouterSocket source = sourcePort.openRouter(sourceRid);
        final RouterSocket target;
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
        final AtomicInteger completedClaimTurns = new AtomicInteger();
        final List<Integer> claimTurns = new java.util.ArrayList<>();
        final ZLinkJavaRawMeshNode node;
        final AtomicLong now = new AtomicLong();
        final java.util.concurrent.atomic.AtomicReference<RuntimeException> managementFailure =
                new java.util.concurrent.atomic.AtomicReference<>();
        final ZLinkServiceTopologyRegistry topology;
        volatile long failClaimCorrelation;
        volatile ZLinkJavaRawServicePort.Inbound failedInbound;
        String sourceConnection;
        final ExecutorService executor = Executors.newSingleThreadExecutor();

        RotationRequests() {
            this(null);
        }

        RotationRequests(RuntimeException disconnectFailure) {
            this(disconnectFailure, true);
        }

        RotationRequests(RuntimeException disconnectFailure, boolean requiresClaim) {
            Context targetContext =
                    new RouterContext(context, disconnectFailure, claims, requiresClaim);
            targetPort =
                    new ZLinkJavaRawServicePort(
                            targetContext,
                            new ZLinkStateLane(),
                            (socket, observesRoutes) ->
                                    new ZLinkJavaSocketReceivePoller(
                                            socket instanceof DisconnectFailureRouter wrapped
                                                    ? wrapped.nativeSocket
                                                    : socket,
                                            true,
                                            observesRoutes));
            target = targetPort.openRouter(targetRid, true);
            var descriptor = admissionDescriptor(targetRid, 1);
            topology = new ZLinkServiceTopologyRegistry(descriptor, targetPort.stateLane());
            node =
                    new ZLinkJavaRawMeshNode(
                            "rotation",
                            System::currentTimeMillis,
                            () -> {
                                if (targetPort.stateLane().isOnLane()) {
                                    var failure = managementFailure.getAndSet(null);
                                    if (failure != null) throw failure;
                                }
                                return now.get();
                            },
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(15),
                            targetPort,
                            Runnable::run,
                            target,
                            descriptor,
                            topology,
                            Map.of());
            String endpoint = "inproc://rotation-requests-" + UUID.randomUUID();
            target.bind(endpoint);
            source.connect(endpoint);
            awaitRoute(sourcePort, source, targetRid);
            awaitRoute(targetPort, target, sourceRid);
            sourcePort.ensureReceivePollerRegistered(source);
            targetPort.ensureReceivePollerRegistered(target);
            sourceConnection =
                    Long.toUnsignedString(
                            targetPort.routesSnapshot(target).stream()
                                    .filter(route -> route.routingId().equals(sourceRid))
                                    .findFirst()
                                    .orElseThrow()
                                    .routeGeneration(),
                            16);
            topology.admit(
                    admissionDescriptor(sourceRid, 1),
                    sourceConnection,
                    ServiceWireConstants.COMMAND_ADMIT);
            node.setApplicationJobQueue(queue);
            node.setApplicationReceiver(receiver(this::enqueue));
        }

        void enqueue(ZLinkMeshDispatchRecord record) {
            // Keep QUEUED permits until the test enters each accepted application job.
            queued.add(ZLinkApplicationJobContext.transferToQueuedJob());
            accepted.add(record);
        }

        CompletableFuture<List<byte[]>> request(long correlation) throws Exception {
            return request(correlation, ServiceWireConstants.COMMAND_NODE_REQUEST);
        }

        CompletableFuture<List<byte[]>> request(long correlation, int command) throws Exception {
            var stateful = new ZLinkServiceM6BWireCodec();
            byte[] header =
                    switch (command) {
                        case ServiceWireConstants.COMMAND_NODE_REQUEST ->
                                codec.encodeNodeRequestHeader(correlation, 0);
                        case ServiceWireConstants.COMMAND_CHANNEL_REQUEST ->
                                codec.encodeChannelRequestHeader(correlation, "rotation", 0);
                        case ServiceWireConstants.COMMAND_SPOT_REQUEST ->
                                stateful.encodeSpotHeader(
                                        true,
                                        0,
                                        correlation,
                                        0,
                                        correlation,
                                        0,
                                        "source-spot",
                                        new ZLinkServiceM6BWireCodec.SpotRouteFence(
                                                "target-spot", 1, targetRid, 1, 1, 1));
                        case ServiceWireConstants.COMMAND_ACTOR_REQUEST ->
                                stateful.encodeActorHeader(
                                        true,
                                        0,
                                        correlation,
                                        0,
                                        correlation,
                                        0,
                                        null,
                                        new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                                new ZLinkBackendActorRef(targetRid, "actor", 1),
                                                1,
                                                1,
                                                1));
                        default -> throw new IllegalArgumentException("not a request command");
                    };
            var admitted = new CompletableFuture<Void>();
            CompletableFuture<List<byte[]>> result;
            try (var packet = Message.from("packet")) {
                result =
                        sourcePort
                                .request(
                                        source,
                                        targetRid,
                                        List.of(
                                                header,
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
                        if (failClaimCorrelation != 0L) {
                            claimTurns.add(completedClaimTurns.get());
                            if (claims.get() == 1) {
                                CompletableFuture.runAsync(
                                                () ->
                                                        targetPort
                                                                .stateLane()
                                                                .runAsync(
                                                                        () -> {
                                                                            completedClaimTurns
                                                                                    .incrementAndGet();
                                                                            return null;
                                                                        }))
                                        .orTimeout(5, TimeUnit.SECONDS)
                                        .join();
                            }
                        }
                        byte[] head = inbound.frames().getFirst();
                        var stateful = new ZLinkServiceM6BWireCodec();
                        Long correlation =
                                switch (codec.decodeHeader(head).command()) {
                                    case ServiceWireConstants.COMMAND_NODE_REQUEST ->
                                            codec.decodeNodeRequestHeader(head);
                                    case ServiceWireConstants.COMMAND_CHANNEL_REQUEST ->
                                            codec.decodeChannelRequestHeader(head).correlation();
                                    case ServiceWireConstants.COMMAND_SPOT_REQUEST ->
                                            stateful.decodeSpotHeader(head).correlation();
                                    case ServiceWireConstants.COMMAND_ACTOR_REQUEST ->
                                            stateful.decodeActorHeader(head).correlation();
                                    default -> null;
                                };
                        boolean fail = correlation != null && correlation == failClaimCorrelation;
                        if (fail) {
                            failedInbound = inbound;
                            topology.admit(
                                    admissionDescriptor(sourceRid, 1),
                                    "invalid-hex",
                                    ServiceWireConstants.COMMAND_ADMIT);
                        }
                        try {
                            return node.claimRecordForTest(inbound);
                        } finally {
                            if (fail)
                                topology.admit(
                                        admissionDescriptor(sourceRid, 1),
                                        sourceConnection,
                                        ServiceWireConstants.COMMAND_ADMIT);
                        }
                    });
        }

        void start() {
            install();
            node.pumpForTest(executor);
            executor.execute(() -> node.pumpTurnForTest(target));
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
    void readablePumpObservesTransportChangesAndPeerCloseBeforeDispatch() throws Exception {
        var localRid = RoutingId.from("management-order-local");
        var peerRid = RoutingId.from("management-order-peer");
        var closingRid = RoutingId.from("management-order-closing");
        var descriptor = admissionDescriptor(localRid, 1);
        try (var context = Zlink.createContext();
                var port = new ZLinkJavaRawServicePort(context);
                var peerPort = new ZLinkJavaRawServicePort(context);
                var router = port.openRouter(localRid, true);
                var peer = peerPort.openRouter(peerRid);
                var node =
                        new ZLinkJavaRawMeshNode(
                                "rotation",
                                System::currentTimeMillis,
                                () -> 0L,
                                Duration.ofSeconds(5),
                                Duration.ofSeconds(15),
                                port,
                                Runnable::run,
                                router,
                                descriptor,
                                new ZLinkServiceTopologyRegistry(descriptor, port.stateLane()),
                                Map.of())) {
            String endpoint = "inproc://management-order-" + UUID.randomUUID();
            peer.bind(endpoint);
            router.connect(endpoint);
            awaitRoute(peerPort, peer, localRid);
            peerPort.send(peer, localRid, List.of(new byte[] {1}))
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            while (!port.waitForReadable(router, Duration.ofNanos(deadline - System.nanoTime())))
                assertTrue(System.nanoTime() < deadline, "sender must be readable");
            assertEquals(0L, node.observePeerRoute(peerRid).selectedGeneration());

            String closingEndpoint = "inproc://management-order-unbound-" + UUID.randomUUID();
            long closingIntent = node.connectPeer(closingEndpoint, closingRid);
            assertThrows(
                    IllegalStateException.class,
                    () ->
                            node.replacePeerConnection(
                                    closingEndpoint,
                                    closingRid,
                                    1,
                                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY));
            assertTrue(node.isPeerConnectionClosing(closingIntent));

            var endRotation = new AssertionError("readable rotation dispatched");
            var claims = new AtomicInteger();
            var owned = new java.util.ArrayList<Received>();
            installClaims(
                    node,
                    () -> {
                        if (claims.getAndIncrement() != 0) return null;
                        return admissionRecord(
                                () -> {
                                    assertFalse(port.stateLane().isOnLane());
                                    assertTrue(
                                            node.observePeerRoute(peerRid).selectedGeneration()
                                                    != 0,
                                            "transport snapshot must precede dispatch");
                                    assertFalse(
                                            node.isPeerConnectionClosing(closingIntent),
                                            "peer-close request must settle before dispatch");
                                    assertFalse(node.hasLivePeerIntent(closingEndpoint));
                                    throw endRotation;
                                },
                                owned);
                    });
            org.junit.jupiter.api.Assertions.assertSame(
                    endRotation,
                    assertThrows(AssertionError.class, () -> node.pumpTurnForTest(router)));
            assertEquals(2, claims.get());
            assertThrows(IllegalStateException.class, owned.getFirst()::parts);
        }
    }

    @Test
    void readableRotationChecksManagementAndClaimsInOneLaneTurnBeforeDispatch() throws Exception {
        var localRid = RoutingId.from("management-turn-local");
        var peerRid = RoutingId.from("management-turn-peer");
        var descriptor = admissionDescriptor(localRid, 1);
        try (var context = Zlink.createContext();
                var port = new ZLinkJavaRawServicePort(context);
                var peerPort = new ZLinkJavaRawServicePort(context);
                var router = port.openRouter(localRid, true);
                var peer = peerPort.openRouter(peerRid);
                var worker = Executors.newVirtualThreadPerTaskExecutor()) {
            String endpoint = "inproc://management-turn-" + UUID.randomUUID();
            peer.bind(endpoint);
            router.connect(endpoint);
            awaitRoute(port, router, peerRid);
            awaitRoute(peerPort, peer, localRid);
            var lane = port.stateLane();
            var managementChecks = new AtomicInteger();
            var claims = new AtomicInteger();
            var nextTurn = new CompletableFuture<Void>();
            var armManagement = new java.util.concurrent.atomic.AtomicBoolean();
            try (var node =
                    new ZLinkJavaRawMeshNode(
                            "rotation",
                            System::currentTimeMillis,
                            () -> {
                                if (lane.isOnLane()
                                        && armManagement.get()
                                        && claims.get() == 0
                                        && managementChecks.incrementAndGet() == 1) {
                                    CompletableFuture.runAsync(
                                                    () ->
                                                            lane.runAsync(
                                                                    () -> {
                                                                        nextTurn.complete(null);
                                                                        return null;
                                                                    }),
                                                    worker)
                                            .orTimeout(5, TimeUnit.SECONDS)
                                            .join();
                                }
                                return 0L;
                            },
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(15),
                            port,
                            Runnable::run,
                            router,
                            descriptor,
                            new ZLinkServiceTopologyRegistry(descriptor, lane),
                            Map.of())) {
                node.livenessForTest().admit(peerRid, "management-turn", 0L);
                var dispatches = new AtomicInteger();
                var owned = new java.util.ArrayList<Received>();
                var endRotation = new AssertionError("readable rotation dispatched");
                installClaims(
                        node,
                        () -> {
                            assertTrue(lane.isOnLane());
                            assertEquals(1, managementChecks.get());
                            assertFalse(
                                    nextTurn.isDone(),
                                    "management and claim must share one state-lane turn");
                            if (claims.getAndIncrement() != 0) return null;
                            return admissionRecord(
                                    () -> {
                                        assertFalse(lane.isOnLane());
                                        assertTrue(
                                                peerPort.waitForReadable(
                                                        peer, Duration.ofSeconds(5)),
                                                "management action must run before dispatch");
                                        try (var probe = peerPort.receive(peer)) {
                                            assertNotNull(probe);
                                            assertEquals(
                                                    ServiceWireConstants.COMMAND_LIVENESS_PROBE,
                                                    new ZLinkServiceM6AWireCodec()
                                                            .decodeHeader(probe.frames().getFirst())
                                                            .command());
                                        }
                                        dispatches.incrementAndGet();
                                        throw endRotation;
                                    },
                                    owned);
                        });
                peerPort.send(peer, localRid, List.of(new byte[] {1}))
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS);
                assertTrue(port.waitForReadable(router, Duration.ofSeconds(5)));
                armManagement.set(true);
                org.junit.jupiter.api.Assertions.assertSame(
                        endRotation,
                        assertThrows(AssertionError.class, () -> node.pumpTurnForTest(router)));
                assertEquals(1, managementChecks.get());
                assertEquals(2, claims.get());
                assertEquals(1, dispatches.get());
                nextTurn.get(5, TimeUnit.SECONDS);
                assertThrows(IllegalStateException.class, owned.getFirst()::parts);
            }
        }
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
                            var yielded = new CompletableFuture<Boolean>();
                            targetPump.execute(() -> yielded.complete(true));
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
