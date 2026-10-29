package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.util.List;
import java.util.stream.Stream;

final class ZLinkServiceTopologyAdmissionRevisionTest {
    static Stream<Arguments> revisionCases() {
        return Stream.of(
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_ADMIT,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_UPDATE,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        true),
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        2,
                        ZLinkServiceNodeDescriptor.State.PREPARING,
                        false,
                        true),
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        2,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        1,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        true,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_UPDATE,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        true,
                        false));
    }

    @ParameterizedTest
    @MethodSource("revisionCases")
    void sameLifecycleRevisionIsCommandOwned(
            int command,
            long revision,
            ZLinkServiceNodeDescriptor.State state,
            boolean changedEndpoint,
            boolean accepted) {
        for (String connection : List.of("route", "replacement")) {
            var registry =
                    new ZLinkServiceTopologyRegistry(
                            descriptor(
                                    "local", 1, ZLinkServiceNodeDescriptor.State.PREPARING, false));
            var original = descriptor("peer", 2, ZLinkServiceNodeDescriptor.State.PREPARING, false);
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    registry.admit(original, "route", ServiceWireConstants.COMMAND_HELLO));
            var incoming = descriptor("peer", revision, state, changedEndpoint);
            var result = registry.admit(incoming, connection, command);
            assertEquals(
                    accepted,
                    result == ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    "command=" + command + " connection=" + connection);
            assertEquals(
                    accepted ? incoming : original,
                    registry.peer(original.nodeRoutingId()).orElseThrow().descriptor());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void updateCannotEstablishAdmission(boolean knownPeer) {
        var registry =
                new ZLinkServiceTopologyRegistry(
                        descriptor("local", 1, ZLinkServiceNodeDescriptor.State.PREPARING, false));
        var original = descriptor("peer", 2, ZLinkServiceNodeDescriptor.State.PREPARING, false);
        if (knownPeer) {
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    registry.admit(original, "route", ServiceWireConstants.COMMAND_HELLO));
        }
        var incoming =
                descriptor(
                        "peer",
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        knownPeer ? 8 : 7);
        assertEquals(
                ZLinkServiceTopologyRegistry.AdmissionResult.STALE_DESCRIPTOR,
                registry.admit(incoming, "replacement", ServiceWireConstants.COMMAND_UPDATE));
        if (knownPeer) {
            var peer = registry.peer(original.nodeRoutingId()).orElseThrow();
            assertEquals(original, peer.descriptor());
            assertEquals("route", peer.connectionId());
        } else {
            assertTrue(registry.peer(incoming.nodeRoutingId()).isEmpty());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void preparedReadyPeerCarriesActualReceiptIncludingFirstAdmitAndLateHello(boolean helloFirst) {
        var lane = new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane();
        try {
            var registry = new ZLinkServiceTopologyRegistry(weightedDescriptor("local", 1), lane);
            var liveness =
                    new ZLinkServiceLivenessRegistry(
                            java.time.Duration.ofSeconds(5),
                            java.time.Duration.ofSeconds(15),
                            lane);
            var peer = weightedDescriptor("peer", 3);
            lane.awaitTurn(
                    () -> {
                        if (helloFirst) {
                            registry.admit(peer, "1", ServiceWireConstants.COMMAND_HELLO);
                            registry.bindLiveness(
                                    peer.nodeRoutingId(),
                                    liveness.admit(peer.nodeRoutingId(), "1", 0));
                            assertTrue(registry.selectReadyChannel("orders").isEmpty());
                        }
                        registry.admit(peer, "1", ServiceWireConstants.COMMAND_ADMIT);
                        var receipt = liveness.admit(peer.nodeRoutingId(), "1", 1);
                        registry.bindLiveness(peer.nodeRoutingId(), receipt);
                        assertSame(
                                receipt,
                                registry.selectReadyChannel("orders").orElseThrow().liveness());
                        var current = registry.peer(peer.nodeRoutingId()).orElseThrow();
                        registry.bindLiveness(peer.nodeRoutingId(), receipt);
                        assertSame(
                                current,
                                registry.peer(peer.nodeRoutingId()).orElseThrow(),
                                "same receipt is a value-preserving no-op");
                        for (int command :
                                new int[] {
                                    ServiceWireConstants.COMMAND_ADMIT,
                                    ServiceWireConstants.COMMAND_HELLO
                                }) {
                            registry.admit(peer, "1", command);
                            assertSame(receipt, liveness.admit(peer.nodeRoutingId(), "1", 2));
                            registry.bindLiveness(peer.nodeRoutingId(), receipt);
                            assertTrue(registry.isReady(peer.nodeRoutingId()));
                            assertSame(
                                    receipt,
                                    registry.selectReadyChannel("orders").orElseThrow().liveness());
                        }
                        return null;
                    });
        } finally {
            lane.closeAsync().toCompletableFuture().join();
        }
    }

    @org.junit.jupiter.api.Test
    void receiptRebuildPreservesTheNextWeightedSelection() {
        var lane = new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane();
        try {
            var expected = new ZLinkServiceTopologyRegistry(weightedDescriptor("local", 1), lane);
            var actual = new ZLinkServiceTopologyRegistry(weightedDescriptor("local", 1), lane);
            var liveness =
                    new ZLinkServiceLivenessRegistry(
                            java.time.Duration.ofSeconds(5),
                            java.time.Duration.ofSeconds(15),
                            lane);
            lane.awaitTurn(
                    () -> {
                        for (var peer :
                                List.of(
                                        weightedDescriptor("a", 5),
                                        weightedDescriptor("b", 3),
                                        weightedDescriptor("c", 1))) {
                            expected.admit(peer, "1", ServiceWireConstants.COMMAND_ADMIT);
                            actual.admit(peer, "1", ServiceWireConstants.COMMAND_ADMIT);
                        }
                        for (int index = 0; index < 128; index++) {
                            if (index == 7) {
                                for (var peer : actual.peers()) {
                                    actual.bindLiveness(
                                            peer.descriptor().nodeRoutingId(),
                                            liveness.admit(
                                                    peer.descriptor().nodeRoutingId(), "1", 0));
                                }
                            }
                            assertEquals(
                                    expected.selectReadyChannel("orders")
                                            .orElseThrow()
                                            .descriptor()
                                            .nodeRoutingId(),
                                    actual.selectReadyChannel("orders")
                                            .orElseThrow()
                                            .descriptor()
                                            .nodeRoutingId(),
                                    "selection " + index);
                        }
                        return null;
                    });
        } finally {
            lane.closeAsync().toCompletableFuture().join();
        }
    }

    private static ZLinkServiceNodeDescriptor weightedDescriptor(String id, int weight) {
        return new ZLinkServiceNodeDescriptor(
                "mesh",
                RoutingId.from(id),
                7,
                1,
                "inproc://" + id,
                List.of(new ZLinkServiceNodeDescriptor.Channel("orders", weight)),
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

    private static ZLinkServiceNodeDescriptor descriptor(
            String id, long revision, ZLinkServiceNodeDescriptor.State state, boolean changed) {
        return descriptor(id, revision, state, changed, 7);
    }

    private static ZLinkServiceNodeDescriptor descriptor(
            String id,
            long revision,
            ZLinkServiceNodeDescriptor.State state,
            boolean changed,
            long lifecycleGeneration) {
        return new ZLinkServiceNodeDescriptor(
                "mesh",
                RoutingId.from(id),
                lifecycleGeneration,
                revision,
                "inproc://" + id + (changed ? "-changed" : ""),
                List.of(),
                state,
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
}
