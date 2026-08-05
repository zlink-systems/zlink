package systems.zlink.framework.spring.internal.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshNodeRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.runtime.internal.binding.spot.MeshMonitorEvent;
import systems.zlink.framework.runtime.internal.binding.spot.MeshMonitorStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeMonitor;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.binding.spot.PeerChannels;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkTopologyReason;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkMeshNodeMonitoringProjection;

final class ZLinkRouteMeshRuntimeServiceTest {
    @Test
    void snapshotMapsNativeStatusAndAdvancesItsSequence() {
        FakeNode node = new FakeNode();
        try (var runtime = runtime(node)) {
            var first = runtime.snapshot("mesh");
            var second = runtime.snapshot("mesh");

            assertEquals("mesh", first.meshName());
            assertEquals(ZLinkTopologyState.READY, first.state());
            assertEquals(1, first.peers().size());
            assertEquals(ZLinkPeerState.READY, first.peers().getFirst().state());
            assertEquals(1, first.channels().size());
            assertTrue(first.channels().getFirst().isReady());
            assertEquals(1, first.channels().getFirst().readyTargetCount());
            assertEquals(first.sequence() + 1, second.sequence());
            assertEquals(first.peers(), List.copyOf(first.peers()));
        }
    }

    @Test
    void snapshotDistinguishesNotRequiredFromNotConnected() {
        FakeNode node = new FakeNode();
        try (var runtime = runtime(node)) {
            node.peerState = MeshPeerState.NOT_REQUIRED;
            var notRequired = runtime.snapshot("mesh").peers().getFirst();
            assertEquals(ZLinkPeerState.NOT_REQUIRED, notRequired.state());
            assertTrue(notRequired.unavailableReason().isEmpty());

            node.peerState = MeshPeerState.CLOSED;
            var notConnected = runtime.snapshot("mesh").peers().getFirst();
            assertEquals(ZLinkPeerState.NOT_CONNECTED, notConnected.state());
        }
    }

    @Test
    void snapshotDegradesWhenARegisteredChannelHasNoReadyTarget() {
        FakeNode node = new FakeNode();
        node.channelWeight = 0;
        node.peerChannelWeight = 0;
        try (var runtime = runtime(node)) {
            var snapshot = runtime.snapshot("mesh");

            assertEquals(ZLinkTopologyState.DEGRADED, snapshot.state());
            assertFalse(snapshot.isReady());
            assertEquals(0, snapshot.channels().getFirst().readyTargetCount());
            assertEquals(
                ZLinkTopologyReason.CAPACITY_EXCEEDED,
                snapshot.placement().unavailableReason().orElseThrow());
        }
    }

    @Test
    void snapshotDegradesOnlyForUnavailableRequiredPeers() {
        FakeNode node = new FakeNode();
        try (var runtime = runtime(node)) {
            node.peerState = MeshPeerState.CONNECTING;
            var connecting = runtime.snapshot("mesh");
            assertEquals(ZLinkTopologyState.DEGRADED, connecting.state());
            assertFalse(connecting.isReady());
            assertFalse(runtime.isReady("mesh"));

            node.peerState = MeshPeerState.CLOSED;
            assertEquals(
                ZLinkTopologyState.DEGRADED,
                runtime.snapshot("mesh").state());

            node.peerState = MeshPeerState.NOT_REQUIRED;
            var notRequired = runtime.snapshot("mesh");
            assertEquals(ZLinkTopologyState.READY, notRequired.state());
            assertTrue(notRequired.isReady());
            assertTrue(runtime.isReady("mesh"));
        }
    }

    @Test
    void observePublishesInitialStateWithoutWaitingForNativeTraffic() throws Exception {
        FakeNode node = new FakeNode();
        try (var runtime = runtime(node)) {
            CountDownLatch received = new CountDownLatch(1);
            AtomicReference<ZLinkMeshNodeSnapshot> status = new AtomicReference<>();
            runtime.observe("mesh", 1).subscribe(new Flow.Subscriber<>() {
                @Override
                public void onSubscribe(Flow.Subscription subscription) {
                    subscription.request(1);
                }

                @Override
                public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> observed) {
                    status.set(observed.status());
                    received.countDown();
                }

                @Override
                public void onError(Throwable throwable) {
                }

                @Override
                public void onComplete() {
                }
            });

            assertTrue(received.await(2, TimeUnit.SECONDS));
            assertEquals(ZLinkTopologyState.READY, status.get().state());
            assertTrue(status.get().isReady());
        }
    }

    @Test
    void snapshotAndPlacementEventProjectCapacityAndActivationChanges() throws Exception {
        FakeNode node = new FakeNode();
        AtomicReference<ZLinkMeshNodeMonitoringProjection> placement =
            new AtomicReference<>(placement(9, 2, 1));
        try (var runtime = runtime(node, placement)) {
            var snapshot = runtime.snapshot("mesh");
            assertEquals(2, snapshot.placement().activeActorCount());
            assertEquals(2, snapshot.placement().activeSpotCount());

            CountDownLatch changed = new CountDownLatch(1);
            CountDownLatch initialized = new CountDownLatch(1);
            AtomicReference<ZLinkMeshNodeSnapshot> changedStatus = new AtomicReference<>();
            runtime.observe("mesh", 8).subscribe(new Flow.Subscriber<>() {
                @Override
                public void onSubscribe(Flow.Subscription subscription) {
                    subscription.request(Long.MAX_VALUE);
                }

                @Override
                public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> observed) {
                    ZLinkMeshNodeSnapshot item = observed.status();
                    if (initialized.getCount() > 0) {
                        initialized.countDown();
                    } else if (item.placement().activeActorCount() == 4) {
                        changedStatus.set(item);
                        changed.countDown();
                    }
                }

                @Override
                public void onError(Throwable throwable) {
                }

                @Override
                public void onComplete() {
                }
            });

            assertTrue(initialized.await(2, TimeUnit.SECONDS));
            placement.set(placement(10, 4, 2));

            assertTrue(changed.await(2, TimeUnit.SECONDS));
            assertEquals(4, changedStatus.get().placement().activeActorCount());
            assertEquals(4, runtime.snapshot("mesh").placement().activeActorCount());
        }
    }

    @Test
    void placementAvailabilityRequiresPositiveWeightAndRemainingCapacity() {
        FakeNode node = new FakeNode();
        AtomicReference<ZLinkMeshNodeMonitoringProjection> placement =
            new AtomicReference<>(actorPlacement(0, 0, 10));
        try (var runtime = runtime(node, placement)) {
            var zeroWeight = runtime.snapshot("mesh").placement();
            assertFalse(zeroWeight.isAvailable());
            assertEquals(
                ZLinkTopologyReason.CAPACITY_EXCEEDED,
                zeroWeight.unavailableReason().orElseThrow());

            placement.set(actorPlacement(100, 10, 10));
            var exhausted = runtime.snapshot("mesh").placement();
            assertFalse(exhausted.isAvailable());
            assertEquals(
                ZLinkTopologyReason.CAPACITY_EXCEEDED,
                exhausted.unavailableReason().orElseThrow());

            placement.set(actorPlacement(100, 9, 10));
            var available = runtime.snapshot("mesh").placement();
            assertTrue(available.isAvailable());
            assertTrue(available.unavailableReason().isEmpty());

            node.peerState = MeshPeerState.CONNECTING;
            var peerDegraded = runtime.snapshot("mesh");
            assertEquals(ZLinkTopologyState.DEGRADED, peerDegraded.state());
            assertTrue(peerDegraded.placement().isAvailable());
            node.peerState = MeshPeerState.ADMITTED;

            placement.set(spotPlacement(100, 5, 5));
            assertFalse(runtime.snapshot("mesh").placement().isAvailable());

            placement.set(spotPlacement(100, 4, 5));
            assertTrue(runtime.snapshot("mesh").placement().isAvailable());

            placement.set(placement(11, 2, 8));
            assertFalse(runtime.snapshot("mesh").placement().isAvailable());
        }
    }

    @Test
    void clientOnlyChannelIncludesRemoteReadyServerWithoutLocalContribution() {
        FakeNode node = new FakeNode();
        node.channelWeight = 0;
        AtomicReference<ZLinkMeshNodeMonitoringProjection> placement =
            new AtomicReference<>(actorPlacement(100, 0, 10));
        try (var runtime = new ZLinkRouteMeshRuntimeService(
            () -> Map.of("mesh", node),
            () -> {
                throw new ZLinkConfigurationException("not configured");
            },
            (meshName, rid) -> placement.get(),
            meshName -> List.of("channel"))) {
            var channel = runtime.snapshot("mesh").channels().getFirst();

            assertTrue(channel.isReady());
            assertEquals(1, channel.readyTargetCount());
        }
    }

    @Test
    void runtimeOptionsDelegatesToCoreRuntimeOptions() {
        var nodeOptions = new ZLinkMeshNodeRuntimeOptions() {
            @Override
            public long maxMessageSize() {
                return 4096;
            }

            @Override
            public void maxMessageSize(long value) {
            }
        };
        var meshChannelOptions = new ZLinkMeshChannelRuntimeOptions() {
            @Override
            public int weight() {
                return 100;
            }

            @Override
            public void weight(int value) {
            }
        };
        var placementOptions = new ZLinkMeshPlacementRuntimeOptions() {
            @Override
            public int placementWeight() {
                return 100;
            }

            @Override
            public void setPlacementWeight(int value) {
            }
        };
        ZLinkRouteMeshRuntimeOptions delegate = new ZLinkRouteMeshRuntimeOptions() {
            @Override
            public ZLinkMeshNodeRuntimeOptions meshNode(String meshName) {
                return nodeOptions;
            }

            @Override
            public ZLinkMeshChannelRuntimeOptions channel(
                String meshName,
                String channelName) {
                return meshChannelOptions;
            }

            @Override
            public ZLinkMeshPlacementRuntimeOptions mesh(String meshName) {
                return placementOptions;
            }

            @Override
            public ZLinkMeshChannelRuntimeOptions channel(String channelName) {
                return meshChannelOptions;
            }
        };
        var options = new ZLinkRouteMeshRuntimeOptionsService(() -> delegate);

        assertSame(nodeOptions, options.meshNode("mesh"));
        assertSame(meshChannelOptions, options.channel("mesh", "channel"));
        assertSame(placementOptions, options.mesh("mesh"));
        assertSame(meshChannelOptions, options.channel("channel"));
    }

    private static ZLinkRouteMeshRuntimeService runtime(FakeNode node) {
        return new ZLinkRouteMeshRuntimeService(
            () -> Map.of("mesh", node),
            () -> {
                throw new ZLinkConfigurationException("not configured");
            });
    }

    private static ZLinkRouteMeshRuntimeService runtime(
        FakeNode node,
        AtomicReference<ZLinkMeshNodeMonitoringProjection> placement) {
        return new ZLinkRouteMeshRuntimeService(
            () -> Map.of("mesh", node),
            () -> {
                throw new ZLinkConfigurationException("not configured");
            },
            (meshName, rid) -> placement.get());
    }

    private static ZLinkMeshNodeMonitoringProjection placement(
        long revision,
        int actorActive,
        int activationActive) {
        return new ZLinkMeshNodeMonitoringProjection(
            revision,
            ZLinkMeshNodeObjectRole.SERVER,
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(actorActive, 1, 0),
                new ZLinkCapacityUsage(2, 3, 10),
                List.of(new ZLinkSpotTypeCapacity(
                    ZLinkPlacementObjectKind.INSTANCE_SPOT,
                    "room",
                    new ZLinkCapacityUsage(2, 1, 5)))),
            new ZLinkActivationConcurrency(activationActive, 8),
            List.of(),
            0,
            java.util.Optional.empty());
    }

    private static ZLinkMeshNodeMonitoringProjection actorPlacement(
        int weight,
        int active,
        int limit) {
        return new ZLinkMeshNodeMonitoringProjection(
            1,
            ZLinkMeshNodeObjectRole.SERVER,
            weight,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(active, 0, limit),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 8),
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                "player",
                ZLinkObjectMaintenancePolicyKind.DISABLED,
                false,
                0)),
            0,
            java.util.Optional.empty());
    }

    private static ZLinkMeshNodeMonitoringProjection spotPlacement(
        int weight,
        int active,
        int limit) {
        ZLinkCapacityUsage usage = new ZLinkCapacityUsage(active, 0, limit);
        return new ZLinkMeshNodeMonitoringProjection(
            1,
            ZLinkMeshNodeObjectRole.SERVER,
            weight,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                usage,
                List.of(new ZLinkSpotTypeCapacity(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "room",
                    usage))),
            new ZLinkActivationConcurrency(0, 8),
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.USER_SPOT,
                "room",
                ZLinkObjectMaintenancePolicyKind.DISABLED,
                false,
                limit)),
            0,
            java.util.Optional.empty());
    }

    private static final class FakeNode implements ZLinkInternalMeshNode {
        private final RoutingId local = RoutingId.from("local");
        private volatile long maxMessageSize;
        private volatile int channelWeight = 7;
        private volatile int peerChannelWeight = 3;
        private volatile int placementWeight = 100;
        private volatile MeshPeerState peerState = MeshPeerState.ADMITTED;
        private final MeshNodeStatus status = new MeshNodeStatus(
            MeshNodeState.READY,
            local,
            "mesh",
            "inproc://mesh",
            3,
            5,
            1,
            1,
            1,
            0,
            2,
            1,
            64,
            0,
            10);

        @Override
        public String name() {
            return "mesh";
        }

        @Override
        public void setBind(String endpoint) {
        }

        @Override
        public void addChannel(String channelName) {
        }

        @Override
        public void setChannelWeight(String channelName, int weight) {
            channelWeight = weight;
        }

        @Override
        public int placementWeight() {
            return placementWeight;
        }

        @Override
        public void setPlacementWeight(int weight) {
            placementWeight = weight;
        }

        @Override
        public long maxMessageSize() {
            return maxMessageSize;
        }

        @Override
        public void setMaxMessageSize(long value) {
            maxMessageSize = value;
        }

        @Override
        public void setRoutingId(RoutingId routingId) {
        }

        @Override
        public void start() {
        }

        @Override
        public long connectPeer(String endpoint) {
            return 1;
        }

        @Override
        public long connectPeer(String endpoint, RoutingId expectedRoutingId) {
            return 1;
        }

        @Override
        public MeshNodeStatus status() {
            return status;
        }

        @Override
        public List<MeshPeerEntry> peers() {
            return List.of(new MeshPeerEntry(
                RoutingId.from("peer"),
                "inproc://peer",
                1,
                MeshPeerSource.MANUAL,
                peerState,
                4,
                8,
                1,
                0,
                10));
        }

        @Override
        public PeerChannels peerChannels(RoutingId peerRid, long lifecycleGeneration) {
            return new PeerChannels(List.of("channel"), List.of(peerChannelWeight));
        }

        @Override
        public Map<String, Integer> channelWeights() {
            return Map.of("channel", channelWeight);
        }

        @Override
        public List<Long> connectionIntentIds() {
            return List.of(1L);
        }

        @Override
        public MeshNodeMonitor openMonitor() {
            return new MeshNodeMonitor() {
                @Override
                public MeshMonitorEvent recv(RecvFlags flags) {
                    return null;
                }

                @Override
                public MeshMonitorStatus status() {
                    return new MeshMonitorStatus(
                        MeshNodeState.READY, 1, 0, 0, 0, 0, 0, 0, 0, 0);
                }

                @Override
                public void close() {
                }
            };
        }

        @Override
        public void startDispatch(Consumer<ZLinkMeshDispatchRecord> receiver) {
        }

        @Override
        public void close() {
        }
    }
}
