package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
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
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

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

    @org.junit.jupiter.api.RepeatedTest(30)
    void terminalSourceRemainsQueryableAndObservableAfterNativeTeardown() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(rid("terminal-source"));
        var runtime = start(options);
        var view = runtime.routeMeshRuntime();
        CountDownLatch initial = new CountDownLatch(1);
        CountDownLatch stopped = new CountDownLatch(1);
        var terminal = new AtomicReference<ZLinkMeshNodeSnapshot>();
        var observationFailure = new AtomicReference<Throwable>();
        try {
            view.observe(MESH, 8)
                    .subscribe(
                            subscriber(
                                    observed -> {
                                        initial.countDown();
                                        if (observed.status().state()
                                                == ZLinkTopologyState.STOPPED) {
                                            terminal.set(observed.status());
                                            stopped.countDown();
                                        }
                                    },
                                    Long.MAX_VALUE,
                                    failure -> {
                                        observationFailure.set(failure);
                                        stopped.countDown();
                                    }));
            assertTrue(initial.await(WAIT.toMillis(), TimeUnit.MILLISECONDS));
        } finally {
            runtime.close();
        }
        org.junit.jupiter.api.Assertions.assertDoesNotThrow(() -> view.snapshot(MESH));
        assertTrue(stopped.await(WAIT.toMillis(), TimeUnit.MILLISECONDS));
        if (observationFailure.get() != null) {
            throw new AssertionError("terminal observation failed", observationFailure.get());
        }
        assertEquals(terminal.get(), view.snapshot(MESH));
        var late = new AtomicReference<ZLinkMeshNodeSnapshot>();
        CountDownLatch delivered = new CountDownLatch(1);
        view.observe(MESH, 8)
                .subscribe(
                        subscriber(
                                observed -> {
                                    late.set(observed.status());
                                    delivered.countDown();
                                },
                                1));
        assertTrue(delivered.await(WAIT.toMillis(), TimeUnit.MILLISECONDS));
        assertEquals(terminal.get(), late.get());
        assertThrows(
                systems.zlink.framework.errors.ZLinkConfigurationException.class,
                () -> view.snapshot("missing-mesh"));
    }

    @Test
    void unchangedSignalsDoNotConsumeOtherMeshSourceSequence() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(rid("source-one"));
        options.addRouteMesh("other-mesh")
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(rid("source-two"));
        try (var runtime = start(options)) {
            var view = (ZLinkRouteMeshRuntimeView) runtime.routeMeshRuntime();
            CountDownLatch initial = new CountDownLatch(2);
            view.observe(MESH, 8)
                    .subscribe(subscriber(ignored -> initial.countDown(), Long.MAX_VALUE));
            view.observe("other-mesh", 8)
                    .subscribe(subscriber(ignored -> initial.countDown(), Long.MAX_VALUE));
            assertTrue(initial.await(WAIT.toMillis(), TimeUnit.MILLISECONDS));
            long first = view.snapshot(MESH).sequence();
            long second = view.snapshot("other-mesh").sequence();
            view.signalAll();
            var firstDelivered = new AtomicReference<ZLinkMeshNodeSnapshot>();
            var secondDelivered = new AtomicReference<ZLinkMeshNodeSnapshot>();
            CountDownLatch dispatched = new CountDownLatch(2);
            view.observe(MESH, 8)
                    .subscribe(
                            subscriber(
                                    observed -> {
                                        firstDelivered.set(observed.status());
                                        dispatched.countDown();
                                    },
                                    1));
            view.observe("other-mesh", 8)
                    .subscribe(
                            subscriber(
                                    observed -> {
                                        secondDelivered.set(observed.status());
                                        dispatched.countDown();
                                    },
                                    1));
            assertTrue(dispatched.await(WAIT.toMillis(), TimeUnit.MILLISECONDS));
            assertEquals(first, firstDelivered.get().sequence());
            assertEquals(second, secondDelivered.get().sequence());
        }
    }

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
                var readyObserved = new CountDownLatch(1);
                var stoppedObserved = new CountDownLatch(1);
                sourceRuntime
                        .routeMeshRuntime()
                        .observe(MESH, 8)
                        .subscribe(
                                subscriber(
                                        observed -> {
                                            if (peerState(observed.status(), targetRid)
                                                    == ZLinkPeerState.READY) {
                                                readyObserved.countDown();
                                            } else if (readyObserved.getCount() == 0) {
                                                stoppedObserved.countDown();
                                            }
                                        },
                                        Long.MAX_VALUE));
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
                assertTrue(readyObserved.await(WAIT.toMillis(), TimeUnit.MILLISECONDS));
                targetRuntime.close();
                assertTrue(stoppedObserved.await(WAIT.toMillis(), TimeUnit.MILLISECONDS));
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
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(sourceRuntime)
                        .toCompletableFuture()
                        .join();
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
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(sourceRuntime)
                    .toCompletableFuture()
                    .join();
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
    void registeredChannelWithoutReadyTargetDoesNotDegradeTopology() throws Exception {
        RoutingId targetRid = rid("view-channel-target");
        var target = new DefaultZLinkFrameworkOptions();
        target.addLocationStore(new ZLinkInMemoryLocationStore());
        var targetNode =
                target.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(targetRid);
        targetNode.channelName("work").server().setWeight(0);
        try (var targetRuntime = start(target)) {
            var source = new DefaultZLinkFrameworkOptions();
            source.addLocationStore(new ZLinkInMemoryLocationStore());
            var sourceNode =
                    source.addRouteMesh(MESH)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(rid("view-channel-source"));
            sourceNode.channelName("work").server().setWeight(0);
            sourceNode.peerConnections().connect(targetRid, endpoint(targetRuntime));
            try (var sourceRuntime = start(source)) {
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(sourceRuntime)
                        .toCompletableFuture()
                        .join();
                var snapshot =
                        awaitSnapshot(
                                sourceRuntime,
                                status -> peerState(status, targetRid) == ZLinkPeerState.READY);

                assertEquals(ZLinkTopologyState.READY, snapshot.state());
                assertTrue(snapshot.isReady());
                assertEquals(0, snapshot.channels().getFirst().readyTargetCount());
                assertTrue(sourceRuntime.routeMeshRuntime().isReady(MESH));
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
    void zeroWeightPlacementUsesCapacityReasonEvenWhenAPeerIsUnavailable() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setPlacementWeight(0);
        node.objects().server();
        node.channelName("work").server();
        node.peerConnections()
                .connect(rid("view-weight-zero-missing"), "tcp://127.0.0.1:" + unusedPort());
        try (var runtime = start(options)) {
            var status =
                    awaitSnapshot(
                            runtime, snapshot -> snapshot.state() == ZLinkTopologyState.DEGRADED);
            assertFalse(status.placement().isAvailable());
            assertEquals(
                    ZLinkTopologyReason.CAPACITY_EXCEEDED,
                    status.placement().unavailableReason().orElseThrow());
        }
    }

    @Test
    void observationStartsWithWeightChangedBeforeSubscription() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh(MESH)
                .listen("tcp://127.0.0.1:0")
                .setPlacementWeight(100)
                .objects()
                .server();
        try (var runtime = start(options)) {
            awaitSnapshot(runtime, status -> status.placement().isAvailable());
            ((ZLinkRouteMeshRuntimeOptions) runtime.routeMeshRuntime())
                    .mesh(MESH)
                    .setPlacementWeight(0);
            CountDownLatch received = new CountDownLatch(1);
            CountDownLatch changed = new CountDownLatch(1);
            AtomicReference<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> first =
                    new AtomicReference<>();
            runtime.routeMeshRuntime()
                    .observe(MESH, 1)
                    .subscribe(
                            subscriber(
                                    observed -> {
                                        if (received.getCount() > 0) {
                                            first.set(observed);
                                            received.countDown();
                                        } else if (observed.status().placement().isAvailable()) {
                                            changed.countDown();
                                        }
                                    },
                                    Long.MAX_VALUE));

            assertTrue(received.await(2, TimeUnit.SECONDS));
            assertFalse(first.get().status().placement().isAvailable());
            assertEquals(
                    ZLinkTopologyReason.CAPACITY_EXCEEDED,
                    first.get().status().placement().unavailableReason().orElseThrow());
            assertEquals(0, first.get().loss().coalescedCount());
            assertEquals(0, first.get().loss().discardedTerminalCount());
            ((ZLinkRouteMeshRuntimeOptions) runtime.routeMeshRuntime())
                    .mesh(MESH)
                    .setPlacementWeight(100);
            assertTrue(changed.await(5, TimeUnit.SECONDS));
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
            CountDownLatch removed = new CountDownLatch(1);
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
                                        } else if (changed.getCount() == 0
                                                && item.placement().activeActorCount() == 0
                                                && item.placement().activeSpotCount() == 0) {
                                            removed.countDown();
                                        }
                                    },
                                    Long.MAX_VALUE));
            assertTrue(initialized.await(2, TimeUnit.SECONDS));

            var createdSpot =
                    runtime.spotManager()
                            .create("view-room")
                            .submit()
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS);
            var createdActor =
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
            var actor =
                    ((systems.zlink.framework.actors.ZLinkActorCreateResult.Created) createdActor)
                            .actor();
            assertTrue(
                    runtime.actorManager()
                            .destroy(actor)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            assertTrue(
                    runtime.spotManager()
                            .close(createdSpot.spot())
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            assertTrue(removed.await(5, TimeUnit.SECONDS));
        }
    }

    /**
     * #1379: a status query rebuilds the RouteMesh status from this process's own descriptor
     * values. It never reads the Location Store, so a held Store response (for example a Store I/O
     * thread that runs the caller) cannot delay the query.
     */
    @Test
    void statusQueryDoesNotReadTheLocationStoreWhileStoreResponsesAreHeld() throws Exception {
        var store = new HeldScanStore(new ZLinkInMemoryProviderLocationStore());
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh(MESH)
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(rid("held-store"))
                .objects()
                .server();
        try (var runtime = start(options)) {
            awaitSnapshot(runtime, status -> status.placement().isAvailable());
            store.holdScansFrom(Thread.currentThread());
            try {
                var held = runtime.routeMeshRuntime().snapshot(MESH);
                assertEquals(0, store.heldScanCount(), "status query read the Location Store");
                assertTrue(held.placement().isAvailable(), held::toString);
            } finally {
                store.release();
            }
        }
    }

    /** Holds the scans one thread issues until the test releases them. */
    private static final class HeldScanStore
            implements systems.zlink.framework.locationprovider.ZLinkLocationStore {
        private final systems.zlink.framework.locationprovider.ZLinkLocationStore delegate;
        private final List<Runnable> held = new java.util.concurrent.CopyOnWriteArrayList<>();
        private volatile Thread holding;

        HeldScanStore(systems.zlink.framework.locationprovider.ZLinkLocationStore delegate) {
            this.delegate = delegate;
        }

        void holdScansFrom(Thread thread) {
            holding = thread;
        }

        int heldScanCount() {
            return held.size();
        }

        void release() {
            holding = null;
            held.forEach(Runnable::run);
        }

        @Override
        public java.util.concurrent.CompletionStage<
                        systems.zlink.framework.locationprovider.ZLinkStoreReadResult>
                read(
                        systems.zlink.framework.locationprovider.ZLinkStoreKey key,
                        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
                                cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public java.util.concurrent.CompletionStage<
                        systems.zlink.framework.locationprovider.ZLinkStoreWriteResult>
                write(
                        systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest request,
                        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
                                cancellation) {
            return delegate.write(request, cancellation);
        }

        @Override
        public java.util.concurrent.CompletionStage<
                        systems.zlink.framework.locationprovider.ZLinkStoreScanResult>
                scan(
                        systems.zlink.framework.locationprovider.ZLinkStoreScanRequest request,
                        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
                                cancellation) {
            if (Thread.currentThread() != holding) {
                return delegate.scan(request, cancellation);
            }
            var result =
                    new java.util.concurrent.CompletableFuture<
                            systems.zlink.framework.locationprovider.ZLinkStoreScanResult>();
            held.add(
                    () ->
                            delegate.scan(request, cancellation)
                                    .whenComplete(
                                            (value, failure) -> {
                                                if (failure == null) result.complete(value);
                                                else result.completeExceptionally(failure);
                                            }));
            return result;
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
        return subscriber(onNext, demand, ignored -> {});
    }

    private static Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> subscriber(
            java.util.function.Consumer<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> onNext,
            long demand,
            java.util.function.Consumer<Throwable> onError) {
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
            public void onError(Throwable throwable) {
                onError.accept(throwable);
            }

            @Override
            public void onComplete() {}
        };
    }
}
