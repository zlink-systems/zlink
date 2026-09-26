package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkTopologyReason;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;

import java.net.ServerSocket;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Predicate;

/**
 * RouteMesh status built by the core {@link ZLinkRouteMeshRuntimeView} for real runtimes: peer
 * states, topology degradation, the initial observed value and placement events (runtime monitoring
 * §5).
 */
final class ZLinkRouteMeshRuntimeViewTest {
    private static final String MESH = "mesh";
    private static final Duration WAIT = Duration.ofSeconds(30);

    @Test
    void readyPeerAndClientOnlyChannelCountTheRemoteServerOnly() throws Exception {
        RoutingId targetRid = rid("view-ready-target");
        var target = new DefaultZLinkFrameworkOptions();
        var targetNode =
                target.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(targetRid);
        targetNode.channelName("work").server();
        try (var targetRuntime = start(target)) {
            var source = new DefaultZLinkFrameworkOptions();
            var sourceNode =
                    source.addRouteMesh(MESH)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(rid("view-ready-source"));
            sourceNode.channelName("work").client();
            sourceNode.peerConnections().connect(targetRid, endpoint(targetRuntime));
            try (var sourceRuntime = start(source)) {
                var snapshot =
                        awaitSnapshot(
                                sourceRuntime,
                                status -> peerState(status, targetRid) == ZLinkPeerState.READY);

                assertEquals(MESH, snapshot.meshName());
                assertEquals(ZLinkTopologyState.READY, snapshot.state());
                assertTrue(snapshot.isReady());
                assertEquals(1, snapshot.peers().size());
                assertTrue(snapshot.peers().getFirst().unavailableReason().isEmpty());
                assertEquals(1, snapshot.channels().size());
                assertTrue(snapshot.channels().getFirst().isReady());
                assertEquals(1, snapshot.channels().getFirst().readyTargetCount());
                assertEquals(snapshot.peers(), List.copyOf(snapshot.peers()));
            }
        }
    }

    @Test
    void notRequiredPeerKeepsTheTopologyReady() throws Exception {
        RoutingId targetRid = rid("view-not-required-target");
        var target = new DefaultZLinkFrameworkOptions();
        target.addLocationStore(new ZLinkInMemoryLocationStore());
        var targetNode =
                target.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(targetRid);
        targetNode.objects().client();
        try (var targetRuntime = start(target)) {
            var source = new DefaultZLinkFrameworkOptions();
            source.addLocationStore(new ZLinkInMemoryLocationStore());
            var sourceNode =
                    source.addRouteMesh(MESH)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(rid("view-not-required-source"));
            sourceNode.objects().client();
            sourceNode.peerConnections().connect(targetRid, endpoint(targetRuntime));
            try (var sourceRuntime = start(source)) {
                var snapshot =
                        awaitSnapshot(
                                sourceRuntime,
                                status ->
                                        peerState(status, targetRid)
                                                == ZLinkPeerState.NOT_REQUIRED);

                ZLinkMeshPeerSnapshot peer = snapshot.peers().getFirst();
                assertTrue(peer.unavailableReason().isEmpty());
                assertEquals(ZLinkTopologyState.READY, snapshot.state());
                assertTrue(snapshot.isReady());
                assertTrue(sourceRuntime.routeMeshRuntime().isReady(MESH));
            }
        }
    }

    @Test
    void unavailableRequiredPeerDegradesTopologyButNotPlacement() throws Exception {
        RoutingId missingRid = rid("view-missing-target");
        var source = new DefaultZLinkFrameworkOptions();
        source.addLocationStore(new ZLinkInMemoryLocationStore());
        var sourceNode =
                source.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(rid("view-required-source"));
        sourceNode.channelName("work").server();
        sourceNode.objects().server();
        sourceNode.peerConnections().connect(missingRid, "tcp://127.0.0.1:" + unusedPort());
        try (var sourceRuntime = start(source)) {
            var snapshot =
                    awaitSnapshot(
                            sourceRuntime,
                            status ->
                                    status.peers().stream()
                                            .anyMatch(peer -> peer.nodeRid().equals(missingRid)));

            ZLinkPeerState state = peerState(snapshot, missingRid);
            assertTrue(
                    state == ZLinkPeerState.CONNECTING || state == ZLinkPeerState.NOT_CONNECTED,
                    state::toString);
            assertEquals(
                    ZLinkTopologyReason.NO_READY_PEER,
                    snapshot.peers().getFirst().unavailableReason().orElseThrow());
            assertEquals(ZLinkTopologyState.DEGRADED, snapshot.state());
            assertFalse(snapshot.isReady());
            assertFalse(sourceRuntime.routeMeshRuntime().isReady(MESH));
            assertTrue(snapshot.placement().isAvailable());
            assertTrue(snapshot.placement().unavailableReason().isEmpty());
        }
    }

    @Test
    void closedPeerIsNotConnectedAndDegradesTopology() throws Exception {
        RoutingId targetRid = rid("view-closed-target");
        var target = new DefaultZLinkFrameworkOptions();
        var targetNode =
                target.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(targetRid);
        targetNode.channelName("work").server();
        var targetRuntime = start(target);
        boolean targetClosed = false;
        try {
            var source = new DefaultZLinkFrameworkOptions();
            var sourceNode =
                    source.addRouteMesh(MESH)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(rid("view-closed-source"));
            sourceNode.channelName("work").client();
            sourceNode.peerConnections().connect(targetRid, endpoint(targetRuntime));
            try (var sourceRuntime = start(source)) {
                awaitSnapshot(
                        sourceRuntime,
                        status -> peerState(status, targetRid) == ZLinkPeerState.READY);

                targetRuntime.close();
                targetClosed = true;
                var snapshot =
                        awaitSnapshot(
                                sourceRuntime,
                                status ->
                                        peerState(status, targetRid) != ZLinkPeerState.READY
                                                && status.state() == ZLinkTopologyState.DEGRADED);

                ZLinkPeerState state = peerState(snapshot, targetRid);
                assertTrue(
                        state == ZLinkPeerState.NOT_CONNECTED || state == ZLinkPeerState.CONNECTING,
                        state::toString);
                assertFalse(snapshot.isReady());
            }
        } finally {
            if (!targetClosed) {
                targetRuntime.close();
            }
        }
    }

    @Test
    void registeredChannelWithoutReadyTargetDegradesTopology() throws Exception {
        RoutingId targetRid = rid("view-channel-target");
        var target = new DefaultZLinkFrameworkOptions();
        var targetNode =
                target.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(targetRid);
        targetNode.channelName("work").server().setWeight(0);
        try (var targetRuntime = start(target)) {
            var source = new DefaultZLinkFrameworkOptions();
            var sourceNode =
                    source.addRouteMesh(MESH)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(rid("view-channel-source"));
            sourceNode.channelName("work").server().setWeight(0);
            sourceNode.peerConnections().connect(targetRid, endpoint(targetRuntime));
            try (var sourceRuntime = start(source)) {
                var snapshot =
                        awaitSnapshot(
                                sourceRuntime,
                                status -> peerState(status, targetRid) == ZLinkPeerState.READY);

                assertEquals(ZLinkTopologyState.DEGRADED, snapshot.state());
                assertFalse(snapshot.isReady());
                assertEquals(0, snapshot.channels().getFirst().readyTargetCount());
            }
        }
    }

    @Test
    void observePublishesInitialStateWithoutWaitingForNativeTraffic() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh(MESH).listen("tcp://127.0.0.1:0");
        try (var runtime = start(options)) {
            awaitSnapshot(runtime, ZLinkMeshNodeSnapshot::isReady);
            CountDownLatch received = new CountDownLatch(1);
            AtomicReference<ZLinkMeshNodeSnapshot> status = new AtomicReference<>();
            runtime.routeMeshRuntime()
                    .observe(MESH, 1)
                    .subscribe(
                            subscriber(
                                    observed -> {
                                        status.set(observed.status());
                                        received.countDown();
                                    },
                                    1));

            assertTrue(received.await(2, TimeUnit.SECONDS));
            assertEquals(ZLinkTopologyState.READY, status.get().state());
            assertTrue(status.get().isReady());
        }
    }

    @Test
    void placementEventsProjectCapacityAndWeightChanges() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setActorCapacity(1)
                        .setSpotCapacity(1);
        node.objects()
                .server()
                .addSpotFactory(
                        "view-room",
                        ZLinkFrameworkLocationRuntimeTest.LocationSpot.class,
                        factory -> factory.disableRelocation())
                .addActorFactory(
                        "view-player",
                        ZLinkFrameworkLocationRuntimeTest.LocationActor.class,
                        ZLinkFrameworkLocationRuntimeTest.LocationActorFactory.class,
                        factory -> factory.disableRelocation());
        try (var runtime = start(options)) {
            var initial =
                    awaitSnapshot(runtime, status -> status.placement().isAvailable()).placement();
            assertEquals(0, initial.activeActorCount());
            assertEquals(0, initial.activeSpotCount());
            assertTrue(initial.unavailableReason().isEmpty());

            var placementOptions = ((ZLinkRouteMeshRuntimeOptions) runtime.routeMeshRuntime());
            placementOptions.mesh(MESH).setPlacementWeight(0);
            var zeroWeight = runtime.routeMeshRuntime().snapshot(MESH).placement();
            assertFalse(zeroWeight.isAvailable());
            assertEquals(
                    ZLinkTopologyReason.CAPACITY_EXCEEDED,
                    zeroWeight.unavailableReason().orElseThrow());
            placementOptions.mesh(MESH).setPlacementWeight(100);

            CountDownLatch initialized = new CountDownLatch(1);
            CountDownLatch changed = new CountDownLatch(1);
            AtomicReference<ZLinkMeshNodeSnapshot> changedStatus = new AtomicReference<>();
            runtime.routeMeshRuntime()
                    .observe(MESH, 8)
                    .subscribe(
                            subscriber(
                                    observed -> {
                                        ZLinkMeshNodeSnapshot item = observed.status();
                                        if (initialized.getCount() > 0) {
                                            initialized.countDown();
                                        } else if (item.placement().activeActorCount() == 1
                                                && item.placement().activeSpotCount() == 1) {
                                            changedStatus.set(item);
                                            changed.countDown();
                                        }
                                    },
                                    Long.MAX_VALUE));
            assertTrue(initialized.await(2, TimeUnit.SECONDS));

            runtime.spotManager()
                    .create("view-room")
                    .submit()
                    .toCompletableFuture()
                    .get(10, TimeUnit.SECONDS);
            runtime.actorManager()
                    .create("view-player-1", "view-player")
                    .submit()
                    .toCompletableFuture()
                    .get(10, TimeUnit.SECONDS);

            assertTrue(changed.await(5, TimeUnit.SECONDS));
            var exhausted = changedStatus.get().placement();
            assertFalse(exhausted.isAvailable());
            assertEquals(
                    ZLinkTopologyReason.CAPACITY_EXCEEDED,
                    exhausted.unavailableReason().orElseThrow());
            assertEquals(
                    1, runtime.routeMeshRuntime().snapshot(MESH).placement().activeSpotCount());
        }
    }

    private static ZLinkFrameworkRuntime start(DefaultZLinkFrameworkOptions options) {
        return ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static RoutingId rid(String prefix) {
        return RoutingId.from(prefix + "-" + Long.toUnsignedString(System.nanoTime(), 36));
    }

    private static String endpoint(ZLinkFrameworkRuntime runtime) {
        return runtime.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
    }

    private static int unusedPort() throws Exception {
        try (var socket = new ServerSocket(0)) {
            return socket.getLocalPort();
        }
    }

    private static ZLinkPeerState peerState(ZLinkMeshNodeSnapshot status, RoutingId peer) {
        return status.peers().stream()
                .filter(candidate -> candidate.nodeRid().equals(peer))
                .map(ZLinkMeshPeerSnapshot::state)
                .findFirst()
                .orElse(null);
    }

    private static ZLinkMeshNodeSnapshot awaitSnapshot(
            ZLinkFrameworkRuntime runtime, Predicate<ZLinkMeshNodeSnapshot> condition)
            throws InterruptedException {
        long deadline = System.nanoTime() + WAIT.toNanos();
        ZLinkMeshNodeSnapshot snapshot = runtime.routeMeshRuntime().snapshot(MESH);
        while (!condition.test(snapshot) && System.nanoTime() < deadline) {
            Thread.sleep(10);
            snapshot = runtime.routeMeshRuntime().snapshot(MESH);
        }
        assertTrue(condition.test(snapshot), snapshot::toString);
        return snapshot;
    }

    private static Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> subscriber(
            java.util.function.Consumer<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> onNext,
            long demand) {
        return new Flow.Subscriber<>() {
            @Override
            public void onSubscribe(Flow.Subscription subscription) {
                subscription.request(demand);
            }

            @Override
            public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> observed) {
                onNext.accept(observed);
            }

            @Override
            public void onError(Throwable throwable) {}

            @Override
            public void onComplete() {}
        };
    }
}
