package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.function.BooleanSupplier;

class ZLinkJavaRawMeshNodePeerProjectionTest {
    private static final Duration OBSERVATION_TIMEOUT = Duration.ofSeconds(5);

    enum Connection {
        RID_ONLY,
        ENDPOINT_ONLY,
        RID_AND_GENERATION
    }

    @ParameterizedTest
    @EnumSource(Connection.class)
    void admittedPeerUsesObservedDescriptor(Connection connection) throws Exception {
        String endpoint = "inproc://peer-projection-" + UUID.randomUUID();
        RoutingId targetRid = RoutingId.from("projection-target");
        try (var context = Zlink.createContext();
                var target = new ZLinkJavaRawMeshNode(context, "mesh");
                var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.addChannel("orders");
            target.addChannel("events");
            source.setRoutingId(RoutingId.from("projection-source"));
            source.setBind(endpoint + "-source");
            source.addChannel("responses");
            target.start();
            source.start();
            long intent =
                    switch (connection) {
                        case RID_ONLY -> source.connectPeer(endpoint, targetRid);
                        case ENDPOINT_ONLY -> source.connectPeer(endpoint);
                        case RID_AND_GENERATION ->
                                source.connectPeer(
                                        endpoint,
                                        targetRid,
                                        target.status().lifecycleGeneration(),
                                        ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
                    };
            await(() -> source.readyChannelMemberCount("orders") == 1);
            target.setChannelWeight("orders", 80);
            await(
                    () ->
                            source.peerChannels(targetRid, target.status().lifecycleGeneration())
                                    .weights()
                                    .contains(80));
            var observed = target.status();
            var peers = source.peers();
            assertEquals(1, peers.size(), "one admitted peer must have one projection");
            var peer = peers.getFirst();
            assertAll(
                    () -> assertEquals(MeshPeerState.ADMITTED, peer.state()),
                    () -> assertNotEquals(0, observed.lifecycleGeneration()),
                    () -> assertEquals(observed.lifecycleGeneration(), peer.lifecycleGeneration()),
                    () -> assertEquals(observed.descriptorRevision(), peer.descriptorRevision()),
                    () -> assertEquals(observed.channelCount(), peer.channelCount()),
                    () ->
                            assertEquals(
                                    List.of("events", "orders"),
                                    source.peerChannels(targetRid, peer.lifecycleGeneration())
                                            .names()),
                    () -> assertEquals(intent, peer.connectionIntentId()),
                    () -> assertEquals(MeshPeerSource.MANUAL, peer.source()),
                    () -> assertEquals(endpoint, peer.endpoint()));
        }
    }

    @Test
    void unobservedIntentDoesNotPublishExpectedGeneration() {
        String endpoint = "inproc://unobserved-projection-" + UUID.randomUUID();
        RoutingId targetRid = RoutingId.from("unobserved-target");
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setBind(endpoint + "-source");
            source.start();
            long intent =
                    source.connectPeer(
                            endpoint,
                            targetRid,
                            -37,
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
            var peer = source.peers().getFirst();
            assertAll(
                    () -> assertEquals(MeshPeerState.CONNECTING, peer.state()),
                    () -> assertEquals(0, peer.lifecycleGeneration()),
                    () -> assertEquals(0, peer.descriptorRevision()),
                    () -> assertEquals(0, peer.channelCount()),
                    () -> assertEquals(intent, peer.connectionIntentId()),
                    () -> assertEquals(MeshPeerSource.MANUAL, peer.source()),
                    () -> assertEquals(targetRid, peer.routingId()),
                    () -> assertEquals(endpoint, peer.endpoint()));
        }
    }

    private static void await(BooleanSupplier condition) throws InterruptedException {
        long deadline = System.nanoTime() + OBSERVATION_TIMEOUT.toNanos();
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline) {
                fail("peer descriptor transition was not observed");
            }
            Thread.sleep(1);
        }
    }
}
