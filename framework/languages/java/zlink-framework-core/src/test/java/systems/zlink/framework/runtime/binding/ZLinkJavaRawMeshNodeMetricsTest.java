package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

final class ZLinkJavaRawMeshNodeMetricsTest {
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
            var fence = new ZLinkServiceM6BWireCodec.SpotRouteFence(
                "spot", 1, pair.target.routingId(), pair.target.lifecycleGeneration(), 1, 1);
            pair.send(List.of(stateful.encodeSpotHeader(
                false, 0, null, 1, 1, 0, "source-spot", fence), invalidPayload));
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
            List<byte[]> reply = pair.port().request(pair.router(), pair.target.routingId(),
                    List.of(wire.encodeNodeRequestHeader(41, 0), new byte[] {0}),
                    Duration.ofSeconds(2))
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertEquals(104, wire.decodeReplyHeader(reply.getFirst()).terminalResult());
            var multicast = new ZLinkServiceM6BWireCodec();
            pair.send(List.of(multicast.encodeLogicalMulticastHeader(
                0, "orders", "topic", "source-spot"), new byte[] {0}));
            pair.port().request(pair.router(), pair.target.routingId(),
                    List.of(wire.encodeNodeRequestHeader(42, 0), new byte[] {0}),
                    Duration.ofSeconds(2))
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertTrue(sink.events.isEmpty());
        }
    }

    @Test
    void instanceActivationFailuresPreserveCapacityAndMissingHandlerReasons() throws Exception {
        RecordingSink sink = new RecordingSink();
        try (var metrics = ZLinkRuntimeMetrics.install(sink);
             Pair pair = new Pair()) {
            var spots = (ZLinkJavaRawSpotNode) pair.target.spotNode();
            spots.registerInstanceSpotType("full", (type, route, spot) ->
                CompletableFuture.failedFuture(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, "activation capacity exhausted")));
            try (Message packet = Message.from("Packet"); Message body = Message.from("body")) {
                pair.source.sendInstanceSpot(pair.instanceRoute("missing"), "unregistered", null,
                        new byte[0], List.of(packet, body))
                    .toCompletableFuture().get(2, TimeUnit.SECONDS);
                sink.expectDrop("instance_spot", "no_handler");
            }
            try (Message packet = Message.from("Packet"); Message body = Message.from("body")) {
                pair.source.sendInstanceSpot(pair.instanceRoute("full"), "full", null,
                        new byte[0], List.of(packet, body))
                    .toCompletableFuture().get(2, TimeUnit.SECONDS);
                sink.expectDrop("instance_spot", "backpressure");
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
            for (var kind : List.of(ZLinkFrameworkErrorKind.REJECTED,
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR)) {
                String type = kind.name();
                spots.registerInstanceSpotType(type, (ignored, route, spot) ->
                    CompletableFuture.failedFuture(new ZLinkFrameworkException(
                        kind, "activation failed")));
                try (Message packet = Message.from("Packet"); Message body = Message.from("body")) {
                    pair.source.sendInstanceSpot(pair.instanceRoute(type), type, null,
                            new byte[0], List.of(packet, body))
                        .toCompletableFuture().get(2, TimeUnit.SECONDS);
                }
                sink.expectDrop("instance_spot", switch (kind) {
                    case SHUTTING_DOWN -> "shutdown";
                    case PROTOCOL_ERROR -> "decode_error";
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
            var stale = new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                0, pair.instanceRoute("stale"), "missing",
                pair.source.lifecycleGeneration() + 1, pair.source.routingId(), null,
                false, 0, 0, null);
            try (Message packet = Message.from("Packet"); Message body = Message.from("body")) {
                pair.send(List.of(stateful.encodeInstanceSpotHeader(stale),
                    new ZLinkServiceM6AWireCodec().encodeFrameworkMultipartFrame(List.of(packet, body))));
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
                    var descriptor = new ZLinkServiceNodeDescriptor(
                        "mesh", peer, 1, reason.equals("not_ready") ? 1 : 2,
                        "inproc://selection-peer",
                        List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
                        reason.equals("not_ready") ? ZLinkServiceNodeDescriptor.State.PREPARING
                            : ZLinkServiceNodeDescriptor.State.DRAINING,
                        current.securityIdentity(), current.applicationVersion(),
                        current.protocolCapabilities(), ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                        100, 1, 0, 0, 0);
                    assertEquals(ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                        topology.admit(descriptor, "connection"));
                    node.admitPeerChannels(peer, Map.of("orders", 100));
                }
                try (Message packet = Message.from("Packet"); Message body = Message.from("body")) {
                    assertThrows(Exception.class, () -> node.sendChannel(
                        "orders", new byte[0], List.of(packet, body)).toCompletableFuture().get());
                    sink.expectSelection(reason);
                }
                try (Message packet = Message.from("Packet"); Message body = Message.from("body")) {
                    assertThrows(Exception.class, () -> node.requestChannel(
                        "orders", new byte[0], List.of(packet, body), Duration.ofSeconds(2))
                        .toCompletableFuture().get());
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
            pair.source.observePeerAdmissionExpectation(discovered, "inproc://discovered", 17,
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
    void readyTopologyMetricsFollowServiceReadinessAndDrainingLifecycle()
        throws Exception {
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
            await(() -> source.peers().stream()
                .anyMatch(peer -> peer.state() == MeshPeerState.ADMITTED));

            assertTrue(source.isPeerTransportConnected(targetRid));
            assertEquals(0, source.readyPeerCount());
            assertEquals(0, source.readyChannelMemberCount("orders"));

            target.markServiceReady();
            await(() -> source.readyPeerCount() == 1
                && source.readyChannelMemberCount("orders") == 1);

            target.markServiceDraining();
            await(() -> source.readyPeerCount() == 0
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

    private static final class Pair implements AutoCloseable {
        private final Context context = Zlink.createContext();
        private final ZLinkJavaRawMeshNode source = node(context, "source");
        private final ZLinkJavaRawMeshNode target = node(context, "target");

        Pair() {
            target.start();
            source.start();
            source.connectPeer(target.status().localEndpoint(), target.routingId());
            await(() -> source.peers().stream().anyMatch(peer -> peer.state() == MeshPeerState.ADMITTED)
                && target.peers().stream().anyMatch(peer -> peer.state() == MeshPeerState.ADMITTED));
        }

        ZLinkJavaRawServicePort port() throws Exception {
            return (ZLinkJavaRawServicePort) field(source, "port");
        }

        RouterSocket router() throws Exception {
            return (RouterSocket) field(source, "router");
        }

        void send(List<byte[]> frames) throws Exception {
            port().send(router(), target.routingId(), frames).toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
        }

        ZLinkServiceM6BWireCodec.InstanceRouteFence instanceRoute(String spotId) {
            return new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                target.routingId(), target.lifecycleGeneration(), spotId, 1,
                "owner", 1, 1, "store");
        }

        @Override
        public void close() {
            source.close();
            target.close();
            context.close();
        }
    }

    private record Event(String name, Map<String, String> tags) { }

    private static final class RecordingSink implements ZLinkRuntimeMetrics.Sink {
        final LinkedBlockingQueue<Event> events = new LinkedBlockingQueue<>();

        @Override
        public void increment(String name, Map<String, String> tags) {
            events.add(new Event(name, tags));
        }

        void expectDrop(String surface, String reason) throws Exception {
            Event event = events.poll(2, TimeUnit.SECONDS);
            assertNotNull(event);
            assertEquals(new Event("zlink.mesh_node.messages.dropped", Map.of(
                "mesh_name", "mesh", "surface", surface,
                "message_kind", "send", "reason", reason)), event);
        }

        void expectSelection(String reason) throws Exception {
            Event event = events.poll(2, TimeUnit.SECONDS);
            assertNotNull(event);
            assertEquals(new Event("zlink.mesh_node.channel.selection_failures", Map.of(
                "mesh_name", "mesh", "channel_name", "orders", "reason", reason)), event);
        }
    }
}
