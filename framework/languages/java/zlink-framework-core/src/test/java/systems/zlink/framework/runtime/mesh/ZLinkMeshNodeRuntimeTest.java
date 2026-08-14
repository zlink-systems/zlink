package systems.zlink.framework.runtime.mesh;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;

class ZLinkMeshNodeRuntimeTest {
    @Test
    void routerDefaultsUseAccountedBytesInsteadOfLegacyMessageCounts() {
        MeshNodeRegistration registration = new MeshNodeRegistration("game");

        assertEquals(4_096_000L,
            registration.configureRouterSocket().sendHighWaterMark());
        assertEquals(4_096_000L,
            registration.configureRouterSocket().receiveHighWaterMark());
    }

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
        registration.objects().client();
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
        registration.configureRouterSocket().setReceiveHighWaterMark(11);
        registration.configureRouterSocket().setSendTimeout(Duration.ofMillis(23));
        registration.configureSpotPublisher().setSendHighWaterMark(91);

        RecordingMeshNode node = new RecordingMeshNode();
        try (ZLinkMeshNodeRuntime ignored = ZLinkMeshNodeRuntime.start(
            registration,
            (context, meshName) -> node,
            new RecordingContext())) {
            assertEquals(7L, node.routerHighWaterMark);
            assertEquals(11L, node.routerReceiveHighWaterMark);
            assertEquals(7, node.pendingAdmissionCapacity);
            assertEquals(Duration.ofMillis(23), node.routerSendTimeout);
        }
    }


    @Test
    void startInstallsIngressOwnersBeforeTheBackendCanReceive() {
        MeshNodeRegistration registration = new MeshNodeRegistration("game");
        registration.listen("inproc://game-ingress-owner");
        registration.channelName("orders").server();
        RecordingMeshNode node = new RecordingMeshNode();
        AtomicInteger immediateDispatches = new AtomicInteger();
        ZLinkMeshApplicationReceiver receiver = new ZLinkMeshApplicationReceiver() {
            @Override
            public void accept(ZLinkMeshDispatchRecord record) {
                try (record) {
                    immediateDispatches.incrementAndGet();
                }
            }

            @Override
            public CompletionStage<Integer> submitLocalNodeSend(
                RoutingId sourceNodeRid,
                byte[] metadata,
                List<systems.zlink.contracts.messaging.Message> parts) {
                return CompletableFuture.completedFuture(0);
            }
        };

        try (ZLinkMeshNodeRuntime ignored = ZLinkMeshNodeRuntime.start(
            registration,
            (context, meshName) -> node,
            new RecordingContext(),
            true,
            receiver)) {
            assertTrue(node.calls.indexOf("application-receiver")
                < node.calls.indexOf("spot-node"));
            assertTrue(node.calls.indexOf("dispatch")
                < node.calls.indexOf("start"));
            assertEquals(1, immediateDispatches.get());
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
        private long routerReceiveHighWaterMark;
        private int pendingAdmissionCapacity;
        private Duration routerSendTimeout;
        private Consumer<ZLinkMeshDispatchRecord> dispatchReceiver;

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
        @Override public void setRouterReceiveHighWaterMark(long value) {
            routerReceiveHighWaterMark = value;
        }
        @Override public void setRouterPendingAdmissionCapacity(int value) {
            pendingAdmissionCapacity = value;
        }
        @Override public void setRouterSendTimeout(Duration value) {
            routerSendTimeout = value;
        }
        @Override public void setApplicationReceiver(
            ZLinkMeshApplicationReceiver value) {
            calls.add("application-receiver");
        }
        @Override public void setRoutingId(RoutingId routingId) {
            calls.add("routing-id:" + routingId);
        }
        @Override public ZLinkInternalSpotNode spotNode() {
            calls.add("spot-node");
            return null;
        }
        @Override public void start() {
            calls.add("start");
            if (dispatchReceiver != null) {
                dispatchReceiver.accept(new ZLinkMeshDispatchRecord(
                    null,
                    null,
                    List.of()));
            }
        }
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
            dispatchReceiver = receiver;
        }
        @Override public void close() { calls.add("close"); }
    }
}
