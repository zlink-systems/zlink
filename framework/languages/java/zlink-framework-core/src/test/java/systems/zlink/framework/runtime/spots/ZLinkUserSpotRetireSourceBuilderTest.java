package systems.zlink.framework.runtime.spots;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationPermitPool;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCompletion;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyOutcome;

final class ZLinkUserSpotRetireSourceBuilderTest {
    private static final ZLinkStoreCancellation NEVER = () -> false;
    private static final String MESH = "retire-source";
    private static final String STABLE_TYPE = "room";
    private static final String SPOT_ID = "room-a";
    private static final RoutingId SOURCE_RID = RoutingId.from("source-node");
    private static final RoutingId TARGET_RID = RoutingId.from("target-node");

    @Test
    void applicationSignaledTurnWithoutRelocationCompletesBeforeNextJob()
        throws Exception {
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        InMemoryRelocationStore relocations = new InMemoryRelocationStore();
        LiveSpot.events.clear();
        try (ZLinkFrameworkRuntime host = ZLinkFrameworkRuntimeTestAccess.start(
                options(locations, relocations, true))) {
            host.spotManager().getOrCreate(SPOT_ID, STABLE_TYPE)
                .submit().toCompletableFuture().get();
            DefaultSpotContext context =
                (DefaultSpotContext) LiveSpot.last.get().context();
            CompletableFuture<Void> releaseTurn = new CompletableFuture<>();
            CompletionStage<Void> first = context.enqueueDispatch(() -> {
                LiveSpot.events.add("turn");
                context.relocationReady().defer();
                assertThrows(
                    ZLinkFrameworkException.class,
                    context::close);
                return releaseTurn;
            });
            CompletionStage<Void> next = context.enqueueDispatch(() -> {
                LiveSpot.events.add("next");
                return CompletableFuture.completedFuture(null);
            });
            releaseTurn.complete(null);
            CompletableFuture.allOf(
                first.toCompletableFuture(),
                next.toCompletableFuture()).get();

            assertEquals(
                List.of("turn", "continued", "next"),
                List.copyOf(LiveSpot.events));
        }
    }

    @Test
    void applicationSignaledPrecommitAbortContinuesBeforeHeldJob()
        throws Exception {
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        InMemoryRelocationStore relocations = new InMemoryRelocationStore();
        LiveSpot.events.clear();
        try (ZLinkFrameworkRuntime host = ZLinkFrameworkRuntimeTestAccess.start(
                options(locations, relocations, true))) {
            host.spotManager().getOrCreate(SPOT_ID, STABLE_TYPE)
                .submit().toCompletableFuture().get();
            ZLinkSpotRuntime runtime = (ZLinkSpotRuntime) host.spotManager();
            DefaultSpotContext context =
                (DefaultSpotContext) LiveSpot.last.get().context();
            ZLinkUserSpotRelocationBarrier barrier =
                context.relocationBarrier(runtime.actorSessions());
            CompletionStage<Optional<ZLinkUserSpotRelocationBarrier.Seal>>
                sealing = barrier.sealForRelocation(
                    ignored -> true,
                    () -> false);
            CompletableFuture<Void> releaseTurn = new CompletableFuture<>();
            CompletionStage<Void> first = context.enqueueDispatch(() -> {
                LiveSpot.events.add("turn");
                context.relocationReady().defer();
                return releaseTurn;
            });
            CompletionStage<Void> held = context.enqueueAcceptedDispatch(
                new byte[] {1},
                () -> {
                    LiveSpot.events.add("held");
                    return CompletableFuture.completedFuture(null);
                },
                () -> { });
            releaseTurn.complete(null);
            ZLinkUserSpotRelocationBarrier.Seal seal =
                sealing.toCompletableFuture().get().orElseThrow();

            assertTrue(barrier.abort(seal));
            CompletableFuture.allOf(
                first.toCompletableFuture(),
                held.toCompletableFuture()).get();

            assertEquals(
                List.of("turn", "continued", "held"),
                List.copyOf(LiveSpot.events));
        }
    }

    @Test
    void anyTurnBoundaryRejectsApplicationDefer() throws Exception {
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        InMemoryRelocationStore relocations = new InMemoryRelocationStore();
        try (ZLinkFrameworkRuntime host = ZLinkFrameworkRuntimeTestAccess.start(
                options(locations, relocations))) {
            host.spotManager().getOrCreate(SPOT_ID, STABLE_TYPE)
                .submit().toCompletableFuture().get();
            DefaultSpotContext context =
                (DefaultSpotContext) LiveSpot.last.get().context();
            CompletionStage<Void> turn = context.enqueueDispatch(() -> {
                var failure = assertThrows(
                    ZLinkFrameworkException.class,
                    () -> context.relocationReady().defer());
                assertEquals(
                    ZLinkFrameworkErrorKind
                        .NOT_CONFIGURED,
                    failure.kind());
                return CompletableFuture.completedFuture(null);
            });
            turn.toCompletableFuture().get();
        }
    }

    @Test
    void capturesLiveSpotAndPreparesVerifiedImmutableRootBeforeSourceCommit()
        throws Exception {
        SnapshotAdapter.captured.set(null);
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        ZLinkLocationRepository repository =
            new ZLinkProviderLocationRepository(locations);
        InMemoryRelocationStore relocations = new InMemoryRelocationStore();
        DefaultZLinkFrameworkOptions options = options(locations, relocations);
        var registration = options.registration();
        var nodeRegistration = registration.meshNodes().getFirst();
        try (ZLinkFrameworkRuntime host =
                ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkSpotRuntime runtime = (ZLinkSpotRuntime) host.spotManager();
            ZLinkMeshNodeDescriptor source = repository.listMeshNodes(
                    MESH,
                    ZLinkPageRequest.firstPage())
                .toCompletableFuture().get().items().stream()
                .filter(value -> value.rid().equals(nodeRegistration.routingId()))
                .findFirst().orElseThrow();
            long sourceGeneration = source.lifecycleGeneration();
            ZLinkLocationOwnerToken targetOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                repository.claimOwnerLease(
                    "target-owner", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            var created = host.spotManager().getOrCreate(SPOT_ID, STABLE_TYPE)
                .submit()
                .toCompletableFuture().get();
            assertNotNull(created.spot());
            assertSame(LiveSpot.last.get(), runtime.spotFor(SPOT_ID));
            repository.updateMeshNode(
                    descriptor(TARGET_RID, 9, targetOwner,
                        "inproc://retire-target"),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();

            ZLinkAggregateRelocationCoordinator coordinator =
                new ZLinkAggregateRelocationCoordinator(
                    repository, relocations);
            ZLinkRelocationPermitPool permits =
                new ZLinkRelocationPermitPool(new ZLinkLocationOptions());
            ZLinkUserSpotRetireSourceBuilder builder =
                new ZLinkUserSpotRetireSourceBuilder(
                    MESH,
                    nodeRegistration.routingId(),
                    sourceGeneration,
                    repository,
                    coordinator,
                    permits,
                    runtime.spotLifecycle(),
                    runtime.actorSessions(),
                    new ZLinkRelocationAdapterRegistry(
                        registration,
                        ZLinkHandlerActivator.reflection()),
                    nodeRegistration.relocatableSpotFactories(),
                    nodeRegistration.relocatableActorFactories());

            ZLinkUserSpotRetireSourceBuilder.PreparedSource prepared =
                builder.prepare(
                    SPOT_ID,
                    rollingToVersionOne(),
                    NEVER).toCompletableFuture().get();

            assertSame(LiveSpot.last.get(), SnapshotAdapter.captured.get());
            assertEquals(1, permits.snapshot().outboundUnits());
            assertSame(LiveSpot.last.get(), runtime.spotFor(SPOT_ID));
            var root = coordinator.readRoot(
                    prepared.stagedRoot().stored().reference(),
                    prepared.stagedRoot().stored().checksumCrc32c(),
                    NEVER)
                .toCompletableFuture().get();
            var envelope = ZLinkCanonicalUserSpotRelocationEnvelope.decode(
                root.payload(),
                TARGET_RID,
                ignored -> LiveSpot.class,
                prepared.stageRequest());
            assertEquals(SPOT_ID, envelope.spotId());
            assertArrayEquals(new byte[] {7, 4, 1}, envelope.spotState());
            assertTrue(envelope.restoreSpotSnapshot());
            assertTrue(envelope.actors().isEmpty());

            var finalPrepared = prepared.freezeAndPrepareFinal(NEVER)
                .toCompletableFuture().get();
            assertEquals(
                prepared.stageRequest().fence().aggregateId(),
                finalPrepared.fence().aggregateId());
            assertSame(LiveSpot.last.get(), runtime.spotFor(SPOT_ID),
                "authority prepare must not publish target-local Ready");

            prepared.abortPrecommit().toCompletableFuture().get();
            assertEquals(0, permits.snapshot().outboundUnits());
            assertSame(LiveSpot.last.get(), runtime.spotFor(SPOT_ID));

            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                repository.removeMeshNode(
                        new ZLinkMeshNodeDescriptorKey(MESH, TARGET_RID),
                        targetOwner)
                    .toCompletableFuture().get());
            SnapshotAdapter.captured.set(null);
            assertThrows(
                CompletionException.class,
                () -> builder.prepare(
                        SPOT_ID,
                        rollingToVersionOne(),
                        NEVER)
                    .toCompletableFuture().join());
            assertNull(SnapshotAdapter.captured.get(),
                "target admission must finish before sealing and Capture");
            assertEquals(0, permits.snapshot().outboundUnits());
            assertSame(LiveSpot.last.get(), runtime.spotFor(SPOT_ID));
        }
    }

    @Test
    void abortPrecommitRestoresQueueOrderAndResumesLanesLast()
        throws Exception {
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        ZLinkLocationRepository repository =
            new ZLinkProviderLocationRepository(locations);
        InMemoryRelocationStore relocations = new InMemoryRelocationStore();
        DefaultZLinkFrameworkOptions options = options(locations, relocations);
        var registration = options.registration();
        var nodeRegistration = registration.meshNodes().getFirst();
        try (ZLinkFrameworkRuntime host =
                ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkSpotRuntime runtime = (ZLinkSpotRuntime) host.spotManager();
            ZLinkMeshNodeDescriptor source = repository.listMeshNodes(
                    MESH,
                    ZLinkPageRequest.firstPage())
                .toCompletableFuture().get().items().stream()
                .filter(value -> value.rid().equals(
                    nodeRegistration.routingId()))
                .findFirst().orElseThrow();
            ZLinkLocationOwnerToken targetOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                repository.claimOwnerLease(
                    "target-owner", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            host.spotManager().getOrCreate(SPOT_ID, STABLE_TYPE)
                .submit().toCompletableFuture().get();
            repository.updateMeshNode(
                    descriptor(TARGET_RID, 9, targetOwner,
                        "inproc://retire-target"),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();

            List<String> events =
                new CopyOnWriteArrayList<>();
            ZLinkAggregateRelocationCoordinator coordinator =
                new ZLinkAggregateRelocationCoordinator(
                    repository,
                    trackingStore(relocations, events, null, null, null));
            ZLinkRelocationPermitPool permits =
                new ZLinkRelocationPermitPool(new ZLinkLocationOptions());
            ZLinkUserSpotRetireSourceBuilder builder =
                new ZLinkUserSpotRetireSourceBuilder(
                    MESH,
                    nodeRegistration.routingId(),
                    source.lifecycleGeneration(),
                    repository,
                    coordinator,
                    permits,
                    runtime.spotLifecycle(),
                    runtime.actorSessions(),
                    new ZLinkRelocationAdapterRegistry(
                        registration,
                        ZLinkHandlerActivator.reflection()),
                    nodeRegistration.relocatableSpotFactories(),
                    nodeRegistration.relocatableActorFactories());

            ZLinkUserSpotRetireSourceBuilder.PreparedSource prepared =
                builder.prepare(
                    SPOT_ID,
                    rollingToVersionOne(),
                    NEVER).toCompletableFuture().get();
            assertEquals(1, permits.snapshot().outboundUnits());

            DefaultSpotContext context =
                (DefaultSpotContext) LiveSpot.last.get().context();
            CompletionStage<Void> second = context.enqueueAcceptedDispatch(
                new byte[] {2},
                () -> {
                    events.add("resumed:second:outbound="
                        + permits.snapshot().outboundUnits());
                    return CompletableFuture.completedFuture(null);
                },
                () -> { });
            CompletionStage<Void> third = context.enqueueAcceptedDispatch(
                new byte[] {3},
                () -> {
                    events.add("resumed:third");
                    return CompletableFuture.completedFuture(null);
                },
                () -> { });
            assertTrue(
                events.stream().noneMatch(
                    event -> event.startsWith("resumed:")),
                "records admitted after the seal must stay held");

            prepared.abortPrecommit().toCompletableFuture().get();

            CompletionStage<Void> fourth = context.enqueueAcceptedDispatch(
                new byte[] {4},
                () -> {
                    events.add("resumed:fourth");
                    return CompletableFuture.completedFuture(null);
                },
                () -> { });
            CompletableFuture.allOf(
                second.toCompletableFuture(),
                third.toCompletableFuture(),
                fourth.toCompletableFuture()).get();

            List<String> observed = List.copyOf(events);
            int discard = observed.lastIndexOf("staged-root-discard");
            int resumed = observed.indexOf("resumed:second:outbound=0");
            assertTrue(discard >= 0,
                "abort must discard the staged root: " + observed);
            assertTrue(resumed >= 0,
                "the permit must be restored before the lanes resume: "
                    + observed);
            assertTrue(discard < resumed,
                "staged root discard must precede lane resume: " + observed);
            assertEquals(
                List.of(
                    "resumed:second:outbound=0",
                    "resumed:third",
                    "resumed:fourth"),
                observed.subList(resumed, observed.size()),
                "restored records keep the original queue order");
            assertEquals(0, permits.snapshot().outboundUnits());
            assertSame(LiveSpot.last.get(), runtime.spotFor(SPOT_ID));
        }
    }

    @Test
    void captureFailureAfterStagedRootDiscardsTheStagedRoot()
        throws Exception {
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        ZLinkLocationRepository repository =
            new ZLinkProviderLocationRepository(locations);
        InMemoryRelocationStore relocations = new InMemoryRelocationStore();
        DefaultZLinkFrameworkOptions options = options(locations, relocations);
        var registration = options.registration();
        var nodeRegistration = registration.meshNodes().getFirst();
        try (ZLinkFrameworkRuntime host =
                ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkSpotRuntime runtime = (ZLinkSpotRuntime) host.spotManager();
            ZLinkMeshNodeDescriptor source = repository.listMeshNodes(
                    MESH,
                    ZLinkPageRequest.firstPage())
                .toCompletableFuture().get().items().stream()
                .filter(value -> value.rid().equals(
                    nodeRegistration.routingId()))
                .findFirst().orElseThrow();
            ZLinkLocationOwnerToken targetOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                repository.claimOwnerLease(
                    "target-owner", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            host.spotManager().getOrCreate(SPOT_ID, STABLE_TYPE)
                .submit().toCompletableFuture().get();
            repository.updateMeshNode(
                    descriptor(TARGET_RID, 9, targetOwner,
                        "inproc://retire-target"),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();

            List<String> events =
                new CopyOnWriteArrayList<>();
            List<String> putReferences =
                new CopyOnWriteArrayList<>();
            List<String> deleteReferences =
                new CopyOnWriteArrayList<>();
            //  The in-memory store ignores cancellation, so a flag raised on
            //  the first staged read-back deterministically fails the first
            //  step after coordinator.stageRoot succeeded.
            AtomicBoolean lateCancel =
                new AtomicBoolean();
            ZLinkAggregateRelocationCoordinator coordinator =
                new ZLinkAggregateRelocationCoordinator(
                    repository,
                    trackingStore(
                        relocations,
                        events,
                        putReferences,
                        deleteReferences,
                        () -> lateCancel.set(true)));
            ZLinkRelocationPermitPool permits =
                new ZLinkRelocationPermitPool(new ZLinkLocationOptions());
            ZLinkUserSpotRetireSourceBuilder builder =
                new ZLinkUserSpotRetireSourceBuilder(
                    MESH,
                    nodeRegistration.routingId(),
                    source.lifecycleGeneration(),
                    repository,
                    coordinator,
                    permits,
                    runtime.spotLifecycle(),
                    runtime.actorSessions(),
                    new ZLinkRelocationAdapterRegistry(
                        registration,
                        ZLinkHandlerActivator.reflection()),
                    nodeRegistration.relocatableSpotFactories(),
                    nodeRegistration.relocatableActorFactories());

            var prepare = builder.prepare(
                    SPOT_ID,
                    rollingToVersionOne(),
                    lateCancel::get)
                .toCompletableFuture();
            Throwable failure = assertThrows(
                Exception.class,
                () -> prepare.get(
                    30, TimeUnit.SECONDS));
            while (failure.getCause() != null
                && !(failure
                    instanceof CancellationException)) {
                failure = failure.getCause();
            }
            assertInstanceOf(
                CancellationException.class, failure);

            assertFalse(putReferences.isEmpty(),
                "the relocation root must have been staged before failing");
            assertTrue(
                deleteReferences.contains(putReferences.getLast()),
                "the staged root manifest must be discarded: puts="
                    + putReferences + " deletes=" + deleteReferences);
            assertEquals(0, permits.snapshot().outboundUnits(),
                "the outbound permit must be released after the abort");
            assertEquals(0, builder.unresolvedPreparationCount());
            assertSame(LiveSpot.last.get(), runtime.spotFor(SPOT_ID));
        }
    }

    private static ZLinkRelocationStore trackingStore(
        InMemoryRelocationStore delegate,
        List<String> events,
        List<String> putReferences,
        List<String> deleteReferences,
        Runnable afterRead) {
        return new ZLinkRelocationStore() {
            @Override public CompletionStage<ZLinkRelocationStored> put(
                byte[] payload,
                Duration retention,
                ZLinkStoreCancellation cancellation) {
                return delegate.put(payload, retention, cancellation)
                    .thenApply(stored -> {
                        if (putReferences != null) {
                            putReferences.add(stored.reference());
                        }
                        return stored;
                    });
            }

            @Override public CompletionStage<ZLinkRelocationReadResult> get(
                String reference,
                ZLinkStoreCancellation cancellation) {
                return delegate.get(reference, cancellation)
                    .thenApply(read -> {
                        if (afterRead != null) {
                            afterRead.run();
                        }
                        return read;
                    });
            }

            @Override public CompletionStage<ZLinkRelocationRenewResult> renew(
                String reference,
                Duration retention,
                ZLinkStoreCancellation cancellation) {
                return delegate.renew(reference, retention, cancellation);
            }

            @Override public CompletionStage<ZLinkRelocationDeleteResult>
                delete(
                    String reference,
                    ZLinkStoreCancellation cancellation) {
                events.add("staged-root-discard");
                if (deleteReferences != null) {
                    deleteReferences.add(reference);
                }
                return delegate.delete(reference, cancellation);
            }
        };
    }

    private static DefaultZLinkFrameworkOptions options(
        systems.zlink.framework.runtime.locations
            .ZLinkInMemoryLocationStore locations,
        systems.zlink.framework.locationprovider.ZLinkRelocationStore relocations) {
        return options(locations, relocations, false);
    }

    private static DefaultZLinkFrameworkOptions options(
        systems.zlink.framework.runtime.locations
            .ZLinkInMemoryLocationStore locations,
        systems.zlink.framework.locationprovider.ZLinkRelocationStore relocations,
        boolean applicationSignaled) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        var mesh = options.addRouteMesh(MESH)
            .setRoutingIdPrefix(SOURCE_RID.toString())
            .listen("inproc://retire-source");
        mesh.channelName(MESH).server();
        mesh.objects().server().addSpotFactory(
            STABLE_TYPE,
            LiveSpot.class,
            factory -> {
                factory.preserveStateWith(SnapshotAdapter.class);
                if (applicationSignaled) {
                    factory.relocationReadiness(
                        systems.zlink.framework.configuration
                            .ZLinkSpotRelocationReadinessMode
                            .APPLICATION_SIGNALED);
                }
            });
        options.validate();
        return options;
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        long lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        String endpoint) {
        return new ZLinkMeshNodeDescriptor(
            MESH,
            rid,
            lifecycleGeneration,
            1,
            endpoint,
            Map.of(MESH, 100),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.USER_SPOT,
                STABLE_TYPE,
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("node-entry-00000000-0000-4000-8000-000000000001"),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 64),
                new ZLinkCapacityUsage(0, 0, 64),
                List.of()),
            new ZLinkActivationConcurrency(0, 64),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            Instant.now());
    }

    private static ZLinkRelocationTargetPolicy rollingToVersionOne() {
        return new ZLinkRelocationTargetPolicy(
            systems.zlink.framework.runtime.host
                .ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
            0,
            Optional.empty(),
            1);
    }

    public static final class LiveSpot implements ZLinkSpot<ZLinkActor> {
        private static final AtomicReference<LiveSpot> last =
            new AtomicReference<>();
        private static final List<String> events =
            new CopyOnWriteArrayList<>();
        private final ZLinkSpotContext context;

        public LiveSpot(ZLinkSpotContext context) {
            this.context = context;
            last.set(this);
        }

        @Override public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId,
            ZLinkMessage request) {
            return CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.accept());
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onRelocationReadyCompleted(
            ZLinkSpotRelocationReadyCompletion completion) {
            events.add(completion.outcome()
                == ZLinkSpotRelocationReadyOutcome.CONTINUED
                    ? "continued"
                    : "relocated");
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class SnapshotAdapter
        implements ZLinkSpotRelocationAdapter<LiveSpot> {
        private static final AtomicReference<LiveSpot> captured =
            new AtomicReference<>();

        @Override
        public CompletionStage<byte[]> capture(
            LiveSpot spot,
            ZLinkRelocationCancellation cancellation) {
            captured.set(spot);
            return CompletableFuture.completedFuture(new byte[] {7, 4, 1});
        }

        @Override
        public CompletionStage<Void> restore(
            LiveSpot spot,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
