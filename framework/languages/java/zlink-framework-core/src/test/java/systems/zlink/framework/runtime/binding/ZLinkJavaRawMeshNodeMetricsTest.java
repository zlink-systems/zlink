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
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkJavaRawMeshNodeMetricsTest {
    @Test
    void notMaterializedInstanceFailuresReportOncePerMessage() throws Exception {
        try (DispatchDiagnostics diagnostics = new DispatchDiagnostics();
                Pair pair = new Pair()) {
            var reporter =
                    new ZLinkDispatchErrorReporter(
                            new ZLinkDispatchOptionsRegistration(),
                            ZLinkHandlerActivator.reflection(),
                            Runnable::run);
            pair.target.setDispatchErrorReporter(reporter);
            long count = 0;
            for (boolean local : List.of(false, true)) {
                for (boolean request : List.of(true, false)) {
                    var route = pair.instanceRoute("absent-" + local + "-" + request);
                    var caller = local ? pair.target : pair.source;
                    try (Message packet = Message.from("Packet");
                            Message body = Message.from("body")) {
                        CompletionStage<?> submitted =
                                request
                                        ? caller.requestInstanceSpot(
                                                route,
                                                "absent",
                                                null,
                                                new byte[0],
                                                List.of(packet, body),
                                                Duration.ofSeconds(2))
                                        : caller.submitInstanceSpotSend(
                                                route,
                                                "absent",
                                                null,
                                                new byte[0],
                                                List.of(packet, body));
                        if (request || local) {
                            var failure =
                                    assertThrows(
                                            java.util.concurrent.ExecutionException.class,
                                            () ->
                                                    submitted
                                                            .toCompletableFuture()
                                                            .get(2, TimeUnit.SECONDS));
                            assertEquals(
                                    ZLinkFrameworkErrorKind.NOT_FOUND,
                                    ((ZLinkFrameworkException) failure.getCause()).kind());
                        } else {
                            submitted.toCompletableFuture().get(2, TimeUnit.SECONDS);
                        }
                        diagnostics.expect(
                                request ? "request" : "send",
                                request ? (local ? "fail_caller" : "reply_error") : "drop",
                                "no_handler");
                        assertEquals(++count, reporter.reportedCount());
                    }
                }
            }
            assertEquals(4, reporter.reportedCount());
            assertTrue(diagnostics.events.isEmpty());
        }
    }

    @Test
    void instanceActivationRecoveryRequestFailureReportsOnceWithoutReply() throws Exception {
        try (DispatchDiagnostics diagnostics = new DispatchDiagnostics();
                Pair pair = new Pair()) {
            var reporter =
                    new ZLinkDispatchErrorReporter(
                            new ZLinkDispatchOptionsRegistration(),
                            ZLinkHandlerActivator.reflection(),
                            Runnable::run);
            pair.target.setDispatchErrorReporter(reporter);
            pair.target.registerInstanceSpotType(
                    "missing",
                    (ignored, route, spot) ->
                            CompletableFuture.failedFuture(
                                    new ZLinkFrameworkException(
                                            ZLinkFrameworkErrorKind.NOT_FOUND,
                                            "recovered activation handler is missing")));
            var route = pair.instanceRoute("recovered-request-missing");
            try (Message packet = Message.from("Packet");
                    Message body = Message.from("body")) {
                var envelope =
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                                route.targetSpotId(),
                                "missing",
                                "mesh",
                                pair.target.routingId(),
                                pair.target.lifecycleGeneration(),
                                Long.toString(pair.target.status().descriptorRevision()),
                                pair.source.routingId(),
                                pair.source.lifecycleGeneration(),
                                java.util.Optional.empty(),
                                true,
                                1,
                                1,
                                1L,
                                System.currentTimeMillis() + 2000,
                                new byte[0],
                                new ZLinkServiceM6AWireCodec()
                                        .encodeFrameworkMultipartFrame(List.of(packet, body)));
                assertThrows(
                        java.util.concurrent.ExecutionException.class,
                        () ->
                                pair.target
                                        .recoverInstanceActivation(envelope, route)
                                        .toCompletableFuture()
                                        .get(2, TimeUnit.SECONDS));
                assertEquals(1, reporter.reportedCount());
                diagnostics.expect("request", "fail_caller", "no_handler");
                assertTrue(diagnostics.events.isEmpty());
            }
        }
    }

    @Test
    void coldInstanceRequestFailsAsCoreRequestWhenTargetDisconnects() throws Exception {
        try (Pair pair = new Pair()) {
            var route =
                    new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                            pair.target.routingId(),
                            pair.target.lifecycleGeneration(),
                            "cold-disconnect",
                            "mesh",
                            "unmaterialized",
                            Long.toString(pair.target.status().descriptorRevision()),
                            System.currentTimeMillis() + 5000);
            pair.router().disconnect(pair.target.status().localEndpoint());
            try (Message packet = Message.from("Packet");
                    Message body = Message.from("body")) {
                var readyFailure =
                        assertThrows(
                                java.util.concurrent.ExecutionException.class,
                                () ->
                                        pair.source
                                                .requestInstanceSpot(
                                                        pair.instanceRoute("ready-disconnect"),
                                                        "unmaterialized",
                                                        null,
                                                        new byte[0],
                                                        List.of(packet, body),
                                                        Duration.ofSeconds(5))
                                                .toCompletableFuture()
                                                .get(2, TimeUnit.SECONDS));
                var coldFailure =
                        assertThrows(
                                java.util.concurrent.ExecutionException.class,
                                () ->
                                        pair.source
                                                .requestInstanceSpot(
                                                        route,
                                                        "unmaterialized",
                                                        null,
                                                        new byte[0],
                                                        List.of(packet, body),
                                                        Duration.ofSeconds(5))
                                                .toCompletableFuture()
                                                .get(2, TimeUnit.SECONDS));
                assertEquals(
                        ((ZLinkFrameworkException) readyFailure.getCause()).kind(),
                        ((ZLinkFrameworkException) coldFailure.getCause()).kind());
            }
        }
    }

    @Test
    void recoveredInstanceRequestRetainsRequestClassificationWithoutSendingReply()
            throws Exception {
        try (Pair pair = new Pair()) {
            long replyRouteId = 43;
            var route = pair.instanceRoute("recovered-reply");
            var handled = new java.util.concurrent.atomic.AtomicInteger();
            ((ZLinkJavaRawSpotNode) pair.target.spotNode())
                    .registerInstanceSpotType(
                            "reply",
                            (type, ignored, spot) -> {
                                spot.onDispatchEvent(
                                        new systems.zlink.framework.runtime.internal.backend
                                                .ZLinkInternalAsyncSpotDispatchHandler() {
                                            @Override
                                            public CompletionStage<Void> handleAsync(
                                                    systems.zlink.framework.runtime.internal.backend
                                                                    .ZLinkBackendSpotDispatchInfo
                                                            info) {
                                                return CompletableFuture.completedFuture(null);
                                            }

                                            @Override
                                            public CompletionStage<Void> handleRoute(
                                                    systems.zlink.framework.runtime.internal.backend
                                                                    .ZLinkBackendReceived
                                                            received) {
                                                assertTrue(received.isRequest());
                                                try (Message reply = Message.from("recovered")) {
                                                    received.reply().accept(List.of(reply));
                                                }
                                                handled.incrementAndGet();
                                                return CompletableFuture.completedFuture(null);
                                            }
                                        });
                                return CompletableFuture.completedFuture(null);
                            });
            try (Message packet = Message.from("Packet");
                    Message body = Message.from("body")) {
                var envelope =
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                                route.targetSpotId(),
                                "reply",
                                "mesh",
                                pair.target.routingId(),
                                pair.target.lifecycleGeneration(),
                                Long.toString(pair.target.status().descriptorRevision()),
                                pair.source.routingId(),
                                pair.source.lifecycleGeneration(),
                                java.util.Optional.empty(),
                                true,
                                0,
                                replyRouteId,
                                replyRouteId,
                                System.currentTimeMillis() + 5000,
                                new byte[0],
                                new ZLinkServiceM6AWireCodec()
                                        .encodeFrameworkMultipartFrame(List.of(packet, body)));
                pair.target
                        .recoverInstanceActivation(envelope, route)
                        .toCompletableFuture()
                        .get(2, TimeUnit.SECONDS);
                assertEquals(1, handled.get());
            }
        }
    }

    @Test
    void instanceActivationRequestFailureReportsOnce() throws Exception {
        try (DispatchDiagnostics diagnostics = new DispatchDiagnostics();
                Pair pair = new Pair()) {
            var reporter =
                    new ZLinkDispatchErrorReporter(
                            new ZLinkDispatchOptionsRegistration(),
                            ZLinkHandlerActivator.reflection(),
                            Runnable::run);
            pair.target.setDispatchErrorReporter(reporter);
            var spots = (ZLinkJavaRawSpotNode) pair.target.spotNode();
            spots.registerInstanceSpotType(
                    "missing",
                    (ignored, route, spot) ->
                            CompletableFuture.failedFuture(
                                    new ZLinkFrameworkException(
                                            ZLinkFrameworkErrorKind.NOT_FOUND,
                                            "activation handler is missing")));
            var route = pair.instanceRoute("request-missing");
            spots.registerInstanceSpotAuthority("missing", route);
            try (Message packet = Message.from("Packet");
                    Message body = Message.from("body")) {
                var failure =
                        assertThrows(
                                java.util.concurrent.ExecutionException.class,
                                () ->
                                        pair.source
                                                .requestInstanceSpot(
                                                        route,
                                                        "missing",
                                                        null,
                                                        new byte[0],
                                                        List.of(packet, body),
                                                        Duration.ofSeconds(2))
                                                .toCompletableFuture()
                                                .get(2, TimeUnit.SECONDS));
                assertEquals(
                        ZLinkFrameworkErrorKind.NOT_FOUND,
                        ((ZLinkFrameworkException) failure.getCause()).kind());
                assertEquals(1, reporter.reportedCount());
                diagnostics.expect("request", "reply_error", "no_handler");
                assertTrue(diagnostics.events.isEmpty());
            }
        }
    }

    @Test
    void instanceActivationUsesCanonicalDispatchReasonsOnce() throws Exception {
        try (DispatchDiagnostics diagnostics = new DispatchDiagnostics();
                Pair pair = new Pair()) {
            var reporter =
                    new ZLinkDispatchErrorReporter(
                            new ZLinkDispatchOptionsRegistration(),
                            ZLinkHandlerActivator.reflection(),
                            Runnable::run);
            pair.target.setDispatchErrorReporter(reporter);
            var spots = (ZLinkJavaRawSpotNode) pair.target.spotNode();
            long diagnosticCount = 0;
            for (var expected :
                    List.of(
                            Map.entry(ZLinkFrameworkErrorKind.NOT_FOUND, "no_handler"),
                            Map.entry(ZLinkFrameworkErrorKind.TYPE_MISMATCH, "handler_exception"),
                            Map.entry(ZLinkFrameworkErrorKind.UNAVAILABLE, "stale_target"),
                            Map.entry(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "invalid_frame"),
                            Map.entry(ZLinkFrameworkErrorKind.SHUTTING_DOWN, "shutdown"))) {
                var kind = expected.getKey();
                String type = kind.name();
                spots.registerInstanceSpotType(
                        type,
                        (ignored, route, spot) ->
                                CompletableFuture.failedFuture(
                                        new ZLinkFrameworkException(kind, "activation failed")));
                var route = pair.instanceRoute(type);
                spots.registerInstanceSpotAuthority(type, route);
                try (Message packet = Message.from("Packet");
                        Message body = Message.from("body")) {
                    pair.source
                            .sendInstanceSpot(route, type, null, new byte[0], List.of(packet, body))
                            .toCompletableFuture()
                            .get(2, TimeUnit.SECONDS);
                }
                diagnostics.expect("send", "drop", expected.getValue());
                assertEquals(++diagnosticCount, reporter.reportedCount());
            }
            assertEquals(5, reporter.reportedCount());
            assertTrue(diagnostics.events.isEmpty());
        }
    }

    @Test
    void malformedOneWayWireRecordsDecodeDropsAtDefaultLogging() throws Exception {
        RecordingSink sink = new RecordingSink();
        try (var metrics = ZLinkRuntimeMetrics.install(sink);
                Pair pair = new Pair()) {
            var wire = new ZLinkServiceM6AWireCodec();
            var stateful = new ZLinkServiceM6BWireCodec();
            byte[] invalidPayload = {0};
            pair.send(List.of(wire.encodeNodeSendHeader(0), invalidPayload));
            sink.expectDrop("node", "decode_error");
            pair.send(List.of(wire.encodeChannelSendHeader("orders", 0), invalidPayload));
            sink.expectDrop("channel", "decode_error");
            var fence =
                    new ZLinkServiceM6BWireCodec.SpotRouteFence(
                            "spot",
                            1,
                            pair.target.routingId(),
                            pair.target.lifecycleGeneration(),
                            1,
                            1);
            pair.send(
                    List.of(
                            stateful.encodeSpotHeader(
                                    false, 0, null, 1, 1, 0, "source-spot", fence),
                            invalidPayload));
            sink.expectDrop("spot", "decode_error");
            assertTrue(sink.events.isEmpty());
        }
    }

    @Test
    void malformedRequestAndLogicalMulticastDoNotCountAsOneWayDrops() throws Exception {
        RecordingSink sink = new RecordingSink();
        try (var metrics = ZLinkRuntimeMetrics.install(sink);
                Pair pair = new Pair()) {
            var wire = new ZLinkServiceM6AWireCodec();
            // The request's terminal reply is a deterministic receive barrier.
            List<byte[]> reply =
                    pair.port()
                            .request(
                                    pair.router(),
                                    pair.target.routingId(),
                                    List.of(wire.encodeNodeRequestHeader(41, 0), new byte[] {0}),
                                    Duration.ofSeconds(2))
                            .toCompletableFuture()
                            .get(2, TimeUnit.SECONDS);
            assertEquals(104, wire.decodeReplyHeader(reply.getFirst()).terminalResult());
            var multicast = new ZLinkServiceM6BWireCodec();
            pair.send(
                    List.of(
                            multicast.encodeLogicalMulticastHeader(
                                    0, "orders", "topic", "source-spot"),
                            new byte[] {0}));
            pair.port()
                    .request(
                            pair.router(),
                            pair.target.routingId(),
                            List.of(wire.encodeNodeRequestHeader(42, 0), new byte[] {0}),
                            Duration.ofSeconds(2))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            assertTrue(sink.events.isEmpty());
        }
    }

    @Test
    void instanceActivationFailuresPreserveUnavailableAndMissingHandlerReasons() throws Exception {
        RecordingSink sink = new RecordingSink();
        try (var metrics = ZLinkRuntimeMetrics.install(sink);
                Pair pair = new Pair()) {
            var spots = (ZLinkJavaRawSpotNode) pair.target.spotNode();
            spots.registerInstanceSpotType(
                    "full",
                    (type, route, spot) ->
                            CompletableFuture.failedFuture(
                                    new ZLinkFrameworkException(
                                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                                            "activation unavailable")));
            try (Message packet = Message.from("Packet");
                    Message body = Message.from("body")) {
                var route = pair.instanceRoute("missing");
                spots.registerInstanceSpotAuthority("unregistered", route);
                pair.source
                        .sendInstanceSpot(
                                route, "unregistered", null, new byte[0], List.of(packet, body))
                        .toCompletableFuture()
                        .get(2, TimeUnit.SECONDS);
                sink.expectDrop("instance_spot", "no_handler");
            }
            try (Message packet = Message.from("Packet");
                    Message body = Message.from("body")) {
                var route = pair.instanceRoute("full");
                spots.registerInstanceSpotAuthority("full", route);
                pair.source
                        .sendInstanceSpot(route, "full", null, new byte[0], List.of(packet, body))
                        .toCompletableFuture()
                        .get(2, TimeUnit.SECONDS);
                sink.expectDrop("instance_spot", "no_handler");
            }
            assertTrue(sink.events.isEmpty());
        }
    }

    @Test
    void instanceActivationRejectionIsNotAssumedToBeBackpressure() throws Exception {
        RecordingSink sink = new RecordingSink();
        try (var metrics = ZLinkRuntimeMetrics.install(sink);
                Pair pair = new Pair()) {
            var spots = (ZLinkJavaRawSpotNode) pair.target.spotNode();
            for (var kind :
                    List.of(
                            ZLinkFrameworkErrorKind.REJECTED,
                            ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            ZLinkFrameworkErrorKind.NOT_FOUND,
                            ZLinkFrameworkErrorKind.TYPE_MISMATCH,
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            ZLinkFrameworkErrorKind.INTERNAL_FAILURE)) {
                String type = kind.name();
                spots.registerInstanceSpotType(
                        type,
                        (ignored, route, spot) ->
                                CompletableFuture.failedFuture(
                                        new ZLinkFrameworkException(kind, "activation failed")));
                try (Message packet = Message.from("Packet");
                        Message body = Message.from("body")) {
                    var route = pair.instanceRoute(type);
                    spots.registerInstanceSpotAuthority(type, route);
                    pair.source
                            .sendInstanceSpot(route, type, null, new byte[0], List.of(packet, body))
                            .toCompletableFuture()
                            .get(2, TimeUnit.SECONDS);
                }
                sink.expectDrop(
                        "instance_spot",
                        switch (kind) {
                            case SHUTTING_DOWN -> "shutdown";
                            case PROTOCOL_ERROR -> "decode_error";
                            case NOT_FOUND, TYPE_MISMATCH -> "stale_target";
                            default -> "no_handler";
                        });
            }
            assertTrue(sink.events.isEmpty());
        }
    }

    @Test
    void instanceAdmissionFenceFailureCountsStaleTargetOnce() throws Exception {
        RecordingSink sink = new RecordingSink();
        try (var metrics = ZLinkRuntimeMetrics.install(sink);
                Pair pair = new Pair()) {
            var stateful = new ZLinkServiceM6BWireCodec();
            var stale =
                    new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                            0,
                            pair.instanceRoute("stale"),
                            true,
                            pair.source.lifecycleGeneration() + 1,
                            pair.source.routingId(),
                            null,
                            false,
                            0,
                            0,
                            null);
            try (Message packet = Message.from("Packet");
                    Message body = Message.from("body")) {
                pair.send(
                        List.of(
                                stateful.encodeInstanceSpotHeader(stale),
                                new ZLinkServiceM6AWireCodec()
                                        .encodeFrameworkMultipartFrame(List.of(packet, body))));
            }
            sink.expectDrop("instance_spot", "stale_target");
            assertTrue(sink.events.isEmpty());
        }
    }

    @Test
    void failedSendAndRequestSelectionsUseOnlySpecifiedReasons() throws Exception {
        RecordingSink sink = new RecordingSink();
        try (var metrics = ZLinkRuntimeMetrics.install(sink);
                var context = Zlink.createContext();
                var node = node(context, "selection")) {
            node.start();
            var topology = (ZLinkServiceTopologyRegistry) field(node, "topology");
            RoutingId peer = RoutingId.from("selection-peer");
            for (String reason : List.of("no_member", "not_ready", "draining")) {
                if (!reason.equals("no_member")) {
                    var current = topology.localDescriptor();
                    var descriptor =
                            new ZLinkServiceNodeDescriptor(
                                    "mesh",
                                    peer,
                                    1,
                                    reason.equals("not_ready") ? 1 : 2,
                                    "inproc://selection-peer",
                                    List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
                                    reason.equals("not_ready")
                                            ? ZLinkServiceNodeDescriptor.State.PREPARING
                                            : ZLinkServiceNodeDescriptor.State.DRAINING,
                                    current.securityIdentity(),
                                    current.applicationVersion(),
                                    current.protocolCapabilities(),
                                    ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                                    100,
                                    1,
                                    0,
                                    0,
                                    0);
                    assertEquals(
                            ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                            topology.admit(descriptor, "connection"));
                }
                try (Message packet = Message.from("Packet");
                        Message body = Message.from("body")) {
                    assertThrows(
                            Exception.class,
                            () ->
                                    node.sendChannel("orders", new byte[0], List.of(packet, body))
                                            .toCompletableFuture()
                                            .get());
                    sink.expectSelection(reason);
                }
                try (Message packet = Message.from("Packet");
                        Message body = Message.from("body")) {
                    assertThrows(
                            Exception.class,
                            () ->
                                    node.requestChannel(
                                                    "orders",
                                                    new byte[0],
                                                    List.of(packet, body),
                                                    Duration.ofSeconds(2))
                                            .toCompletableFuture()
                                            .get());
                    sink.expectSelection(reason);
                }
            }
            assertTrue(sink.events.isEmpty());
        }
    }

    @Test
    void configuredAndConnectedReadExistingDescriptorAndTransportState() throws Exception {
        try (Pair pair = new Pair()) {
            assertEquals(List.of(pair.target.routingId()), pair.source.configuredPeerIds());
            assertTrue(pair.source.isPeerTransportConnected(pair.target.routingId()));
            RoutingId discovered = RoutingId.from("discovered-before-connect");
            pair.source.observePeerAdmissionExpectation(
                    discovered,
                    "inproc://discovered",
                    17,
                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
            assertTrue(pair.source.configuredPeerIds().contains(discovered));
            assertFalse(pair.source.isPeerTransportConnected(discovered));
            pair.source.forgetPeerAdmissionExpectation(discovered);
            assertFalse(pair.source.configuredPeerIds().contains(discovered));
            pair.target.close();
            await(() -> !pair.source.isPeerTransportConnected(pair.target.routingId()));
        }
    }

    @Test
    void readyTopologyMetricsFollowServiceReadinessAndDrainingLifecycle() throws Exception {
        RoutingId targetRid = RoutingId.from("metrics-ready-target");
        String endpoint = "inproc://metrics-ready-" + java.util.UUID.randomUUID();
        try (var context = Zlink.createContext();
                var target = new ZLinkJavaRawMeshNode(context, "mesh");
                var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setObjectRole(ZLinkMeshNodeObjectRole.SERVER);
            target.addChannel("orders");
            target.setChannelWeight("orders", 100);
            target.deferServiceReadyPublication();
            source.setRoutingId(RoutingId.from("metrics-ready-source"));
            source.setBind("inproc://metrics-ready-source-" + java.util.UUID.randomUUID());

            target.start();
            source.start();
            source.connectPeer(endpoint, targetRid);
            await(
                    () ->
                            source.peers().stream()
                                    .anyMatch(peer -> peer.state() == MeshPeerState.ADMITTED));

            assertTrue(source.isPeerTransportConnected(targetRid));
            assertEquals(0, source.readyPeerCount());
            assertEquals(0, source.readyChannelMemberCount("orders"));

            target.markServiceReady();
            await(
                    () ->
                            source.readyPeerCount() == 1
                                    && source.readyChannelMemberCount("orders") == 1);

            target.markServiceDraining();
            await(
                    () ->
                            source.readyPeerCount() == 0
                                    && source.readyChannelMemberCount("orders") == 0);
        }
    }

    private static ZLinkJavaRawMeshNode node(Context context, String name) {
        var node = new ZLinkJavaRawMeshNode(context, "mesh");
        node.setRoutingId(RoutingId.from(name));
        node.setBind("inproc://metrics-" + name + "-" + java.util.UUID.randomUUID());
        return node;
    }

    private static Object field(Object target, String name) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    private static void await(BooleanSupplier condition) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() - deadline >= 0) {
                throw new AssertionError("runtime state transition was not observed");
            }
            Thread.yield();
        }
    }

    private static final class DispatchDiagnostics extends Handler implements AutoCloseable {
        private final Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        private final LinkedBlockingQueue<String> events = new LinkedBlockingQueue<>();

        private DispatchDiagnostics() {
            logger.addHandler(this);
        }

        @Override
        public void publish(LogRecord record) {
            if (record.getMessage().contains("event_id=zlink.dispatch_error")) {
                events.add(record.getMessage());
            }
        }

        private void expect(String kind, String action, String reason) throws Exception {
            String diagnostic = events.poll(2, TimeUnit.SECONDS);
            assertNotNull(diagnostic);
            assertTrue(diagnostic.contains("surface=instance_spot"), diagnostic);
            assertTrue(diagnostic.contains("kind=" + kind), diagnostic);
            assertTrue(diagnostic.contains("action=" + action), diagnostic);
            assertTrue(diagnostic.contains("reason=" + reason), diagnostic);
        }

        @Override
        public void flush() {}

        @Override
        public void close() {
            logger.removeHandler(this);
        }
    }

    private static final class Pair implements AutoCloseable {
        private final Context context = Zlink.createContext();
        private final ZLinkJavaRawMeshNode source = node(context, "source");
        private final ZLinkJavaRawMeshNode target = node(context, "target");

        Pair() {
            target.start();
            source.start();
            source.connectPeer(target.status().localEndpoint(), target.routingId());
            await(
                    () ->
                            source.peers().stream()
                                            .anyMatch(
                                                    peer -> peer.state() == MeshPeerState.ADMITTED)
                                    && target.peers().stream()
                                            .anyMatch(
                                                    peer ->
                                                            peer.state()
                                                                    == MeshPeerState.ADMITTED));
        }

        ZLinkJavaRawServicePort port() throws Exception {
            return (ZLinkJavaRawServicePort) field(source, "port");
        }

        RouterSocket router() throws Exception {
            return (RouterSocket) field(source, "router");
        }

        void send(List<byte[]> frames) throws Exception {
            port().send(router(), target.routingId(), frames)
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
        }

        ZLinkServiceM6BWireCodec.InstanceRouteFence instanceRoute(String spotId) {
            return new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                    target.routingId(),
                    target.lifecycleGeneration(),
                    spotId,
                    1,
                    "owner",
                    1,
                    1,
                    "store");
        }

        @Override
        public void close() {
            source.close();
            target.close();
            context.close();
        }
    }

    private record Event(String name, Map<String, String> tags) {}

    private static final class RecordingSink implements ZLinkRuntimeMetrics.Sink {
        final LinkedBlockingQueue<Event> events = new LinkedBlockingQueue<>();

        @Override
        public void increment(String name, Map<String, String> tags) {
            events.add(new Event(name, tags));
        }

        void expectDrop(String surface, String reason) throws Exception {
            Event event = events.poll(2, TimeUnit.SECONDS);
            assertNotNull(event);
            assertEquals(
                    new Event(
                            "zlink.mesh_node.messages.dropped",
                            Map.of(
                                    "mesh_name",
                                    "mesh",
                                    "surface",
                                    surface,
                                    "message_kind",
                                    "send",
                                    "reason",
                                    reason)),
                    event);
        }

        void expectSelection(String reason) throws Exception {
            Event event = events.poll(2, TimeUnit.SECONDS);
            assertNotNull(event);
            assertEquals(
                    new Event(
                            "zlink.mesh_node.channel.selection_failures",
                            Map.of(
                                    "mesh_name",
                                    "mesh",
                                    "channel_name",
                                    "orders",
                                    "reason",
                                    reason)),
                    event);
        }
    }
}
