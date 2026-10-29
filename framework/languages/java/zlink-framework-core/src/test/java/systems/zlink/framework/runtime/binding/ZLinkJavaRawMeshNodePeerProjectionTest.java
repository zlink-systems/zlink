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
            assertEquals(0, source.readyChannelMemberCount(null));
            assertEquals(0, source.readyChannelMemberCount(" "));
            source.setBind(endpoint + "-source");
            source.start();
            assertThrows(
                    IllegalArgumentException.class, () -> source.readyChannelMemberCount(null));
            assertThrows(IllegalArgumentException.class, () -> source.readyChannelMemberCount(" "));

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

    @Test
    void admissionObservationsDoNotWaitForTheOccupiedTransportOwnerAndClosePublishesEmpty()
            throws Exception {
        var localRid = RoutingId.from("root-local");
        var remoteRid = RoutingId.from("root-remote");
        try (var context = Zlink.createContext()) {
            var port = new ZLinkJavaRawServicePort(context);
            var local = observationDescriptor(localRid);
            var remote = observationDescriptor(remoteRid);
            var topology =
                    new systems.zlink.framework.runtime.internal.service
                            .ZLinkServiceTopologyRegistry(local, port.stateLane());
            topology.admit(
                    remote,
                    "7",
                    systems.zlink.framework.runtime.protocol.ServiceWireConstants.COMMAND_ADMIT);
            try (var node =
                    ZLinkJavaRawMeshNodeTesting.create(
                            "mesh",
                            port,
                            null,
                            local,
                            topology,
                            java.util.Map.of(remoteRid, 7L),
                            Runnable::run)) {
                var entered = new java.util.concurrent.CountDownLatch(1);
                var release = new java.util.concurrent.CountDownLatch(1);
                var occupied =
                        port.stateLane()
                                .runAsync(
                                        () -> {
                                            entered.countDown();
                                            try {
                                                assertTrue(
                                                        release.await(
                                                                2,
                                                                java.util.concurrent.TimeUnit
                                                                        .SECONDS));
                                            } catch (InterruptedException failure) {
                                                throw new AssertionError(failure);
                                            }
                                            return null;
                                        });
                assertTrue(entered.await(1, java.util.concurrent.TimeUnit.SECONDS));
                try {
                    assertTimeoutPreemptively(
                            Duration.ofSeconds(1),
                            () -> {
                                assertEquals(1, node.status().admittedPeerCount());
                                assertEquals(
                                        MeshPeerState.ADMITTED, node.peers().getFirst().state());
                                assertEquals(1, node.readyPeerCount());
                                assertEquals(1, node.readyChannelMemberCount("orders"));
                                assertTrue(node.isCanonicalRelocationTargetAdmitted(remoteRid, 7));
                                assertTrue(node.observePeerRoute(remoteRid).ready());
                                assertEquals(
                                        List.of("orders"), node.peerChannels(remoteRid, 0).names());
                                assertTrue(node.configuredPeerIds().contains(remoteRid));
                            });
                } finally {
                    release.countDown();
                }
                occupied.toCompletableFuture().get(1, java.util.concurrent.TimeUnit.SECONDS);
                node.close();
                assertTrue(node.peers().isEmpty());
                assertEquals(0, node.readyPeerCount());
                assertEquals(0, node.readyChannelMemberCount("orders"));
                assertFalse(node.observePeerRoute(remoteRid).ready());
                assertEquals(0, node.observePeerRoute(remoteRid).selectedGeneration());
                assertFalse(node.configuredPeerIds().contains(remoteRid));
            }
        }
    }

    @Test
    void injectedDescriptorAndSelectedRouteRemainObservableWithoutTopology() {
        var localRid = RoutingId.from("unavailable-local");
        var remoteRid = RoutingId.from("unavailable-remote");
        try (var context = Zlink.createContext()) {
            var port = new ZLinkJavaRawServicePort(context);
            var local = observationDescriptor(localRid);
            try (var node =
                    ZLinkJavaRawMeshNodeTesting.create(
                            "mesh",
                            port,
                            null,
                            local,
                            null,
                            java.util.Map.of(remoteRid, 9L),
                            Runnable::run)) {
                assertEquals(local.lifecycleGeneration(), node.status().lifecycleGeneration());
                assertEquals(local.descriptorRevision(), node.status().descriptorRevision());
                assertEquals(9L, node.observePeerRoute(remoteRid).selectedGeneration());
                assertTrue(node.isPeerTransportConnected(remoteRid));
                assertFalse(node.observePeerRoute(remoteRid).ready());
                assertEquals(0, node.readyChannelMemberCount(null));
                assertEquals(0, node.readyChannelMemberCount(" "));
            }
        }
    }

    private static ZLinkServiceNodeDescriptor observationDescriptor(RoutingId rid) {
        return new ZLinkServiceNodeDescriptor(
                "mesh",
                rid,
                7,
                1,
                "inproc://" + rid,
                List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 3)),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                1,
                100,
                10,
                0,
                0);
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
