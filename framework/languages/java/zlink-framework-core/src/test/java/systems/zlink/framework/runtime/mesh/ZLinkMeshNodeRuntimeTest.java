package systems.zlink.framework.runtime.mesh;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;
import java.time.Duration;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

class ZLinkMeshNodeRuntimeTest {
    @Test
    void automaticRoutingIdUsesStablePrefixAndCanonicalUuid() {
        MeshNodeRegistration registration = new MeshNodeRegistration("game");
        registration.setRoutingIdPrefix("play");

        RoutingId first = registration.routingId();

        assertEquals(first, registration.routingId());
        assertTrue(first.toString().matches(
            "play-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"));
    }

    @Test
    void startAppliesIdentityTopologyAndPeersBeforeOwningLifecycle() {
        MeshNodeRegistration registration = new MeshNodeRegistration("game");
        registration.setRoutingIdPrefix("game-1");
        registration.setPlacementWeight(300);
        registration.listen("inproc://game-1");
        registration.channelName("orders").server().setWeight(2);
        registration.peerConnections().connect(
            RoutingId.from("game-2"),
            "inproc://game-2");

        RecordingMeshNode node = new RecordingMeshNode();
        try (ZLinkMeshNodeRuntime ignored = ZLinkMeshNodeRuntime.start(
            registration,
            (context, meshName) -> {
                assertEquals("game", meshName);
                return node;
            },
            new RecordingContext())) {
            assertEquals(List.of(
                "routing-id:" + registration.routingId(),
                "bind:inproc://game-1",
                "placement-weight:300",
                "channel:orders",
                "weight:orders:2",
                "spot-node",
                "start",
                "peer:game-2:inproc://game-2"), node.calls);
        }

        assertEquals("close", node.calls.getLast());
    }

    @Test
    void startRoutesRouterAdmissionSettingsToTheMeshBackend() {
        MeshNodeRegistration registration = new MeshNodeRegistration("game");
        registration.listen("inproc://game-1");
        registration.configureRouterSocket().setSendHighWaterMark(7);
        registration.configureRouterSocket().setSendTimeout(Duration.ofMillis(23));
        registration.configureSpotPublisher().setSendHighWaterMark(91);

        RecordingMeshNode node = new RecordingMeshNode();
        try (ZLinkMeshNodeRuntime ignored = ZLinkMeshNodeRuntime.start(
            registration,
            (context, meshName) -> node,
            new RecordingContext())) {
            assertEquals(7L, node.routerHighWaterMark);
            assertEquals(7, node.pendingAdmissionCapacity);
            assertEquals(Duration.ofMillis(23), node.routerSendTimeout);
        }
    }

    @Test
    void startInstallsApplicationDispatchBudgetBeforeBackendStart() {
        MeshNodeRegistration registration = new MeshNodeRegistration("game");
        registration.listen("inproc://game-budget");
        RecordingMeshNode node = new RecordingMeshNode();
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(1024);

        try (ZLinkMeshNodeRuntime ignored = ZLinkMeshNodeRuntime.start(
            registration,
            (context, meshName) -> node,
            new RecordingContext(),
            budget)) {
            assertSame(budget, node.applicationDispatchBudget);
            assertTrue(node.calls.indexOf("application-budget")
                < node.calls.indexOf("start"));
        }
    }

    private static final class RecordingContext implements ZLinkBackendContext {
        @Override public String name() { return "context"; }
        @Override public void shutdown() { }
        @Override public void close() { }
    }

    private static final class RecordingMeshNode implements ZLinkInternalMeshNode {
        private final List<String> calls = new ArrayList<>();
        private long routerHighWaterMark;
        private int pendingAdmissionCapacity;
        private Duration routerSendTimeout;
        private ZLinkInboundDispatchBudget applicationDispatchBudget;

        @Override public String name() { return "mesh-node"; }
        @Override public void setBind(String endpoint) { calls.add("bind:" + endpoint); }
        @Override public void addChannel(String channelName) {
            calls.add("channel:" + channelName);
        }
        @Override public void setChannelWeight(String channelName, int weight) {
            calls.add("weight:" + channelName + ":" + weight);
        }
        @Override public void setPlacementWeight(int weight) {
            calls.add("placement-weight:" + weight);
        }
        @Override public void setRouterHighWaterMark(long value) {
            routerHighWaterMark = value;
        }
        @Override public void setRouterPendingAdmissionCapacity(int value) {
            pendingAdmissionCapacity = value;
        }
        @Override public void setRouterSendTimeout(Duration value) {
            routerSendTimeout = value;
        }
        @Override public void setApplicationDispatchBudget(
            ZLinkInboundDispatchBudget value) {
            applicationDispatchBudget = value;
            calls.add("application-budget");
        }
        @Override public void setRoutingId(RoutingId routingId) {
            calls.add("routing-id:" + routingId);
        }
        @Override public ZLinkInternalSpotNode spotNode() {
            calls.add("spot-node");
            return null;
        }
        @Override public void start() { calls.add("start"); }
        @Override public long connectPeer(String endpoint) {
            calls.add("peer:" + endpoint);
            return 1L;
        }
        @Override public long connectPeer(String endpoint, RoutingId expectedRoutingId) {
            calls.add("peer:" + expectedRoutingId + ":" + endpoint);
            return 1L;
        }
        @Override public MeshNodeStatus status() {
            return new MeshNodeStatus(
                MeshNodeState.READY,
                RoutingId.from("game-1"),
                "game",
                "inproc://game-1",
                1L,
                1L,
                1,
                1,
                1,
                0,
                0L,
                0L,
                0L,
                0,
                0L);
        }
        @Override public List<MeshPeerEntry> peers() { return List.of(); }
        @Override public List<Long> connectionIntentIds() { return List.of(1L); }
        @Override public void startDispatch(Consumer<ZLinkMeshDispatchRecord> receiver) {
            calls.add("dispatch");
        }
        @Override public void close() { calls.add("close"); }
    }
}
