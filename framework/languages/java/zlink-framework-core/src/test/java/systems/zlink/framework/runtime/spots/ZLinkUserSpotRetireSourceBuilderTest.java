package systems.zlink.framework.runtime.spots;
import java.lang.reflect.Proxy;
import java.lang.reflect.InvocationTargetException;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationPermitPool;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
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
    private static final String ACTOR_TYPE = "player";
    private static final String ACTOR_ID = "player-a";
    private static final String SECOND_ACTOR_ID = "player-b";
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
    void postFreezeIngressBeyondNormalCapacityIsDurableBeforeAckAndReleasedAfterAck()
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
            host.spotManager().getOrCreate(SPOT_ID, STABLE_TYPE)
                .submit().toCompletableFuture().get();
            ZLinkMeshNodeDescriptor source = repository.listMeshNodes(
                    MESH, ZLinkPageRequest.firstPage())
                .toCompletableFuture().get().items().stream()
                .filter(value -> value.rid().equals(
                    nodeRegistration.routingId()))
                .findFirst().orElseThrow();
            ZLinkLocationOwnerToken targetOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                repository.claimOwnerLease(
                        "post-freeze-target", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            repository.updateMeshNode(
                    descriptor(
                        TARGET_RID,
                        9,
                        targetOwner,
                        "inproc://retire-post-freeze-target"),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();

            var coordinator = new ZLinkAggregateRelocationCoordinator(
                repository, relocations);
            var permits = new ZLinkRelocationPermitPool(
                new ZLinkLocationOptions());
            var builder = new ZLinkUserSpotRetireSourceBuilder(
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
                nodeRegistration.relocatableActorFactories(),
                runtime);
            var prepared = builder.prepare(
                    SPOT_ID, rollingToVersionOne(), NEVER)
                .toCompletableFuture().get();
            var finalPrepared = prepared.freezeAndPrepareFinal(NEVER)
                .toCompletableFuture().get();

            DefaultSpotContext context =
                (DefaultSpotContext) LiveSpot.last.get().context();
            List<CompletableFuture<Void>> accepted = new ArrayList<>();
            AtomicInteger released = new AtomicInteger();
            for (int index = 0; index < 1025; index++) {
                byte[] record = ZLinkAcceptedJournalTestRecords.spot(
                    SPOT_ID,
                    SPOT_ID,
                    0,
                    "post-freeze-" + index,
                    Map.of(),
                    new byte[] {(byte) index});
                accepted.add(context.enqueueAcceptedDispatch(
                        record,
                        () -> fail(
                            "source must not execute transferred ingress"),
                        released::incrementAndGet)
                    .toCompletableFuture());
            }
            var published = prepared.commitAuthority(NEVER)
                .toCompletableFuture().get();
            CompletableFuture<Void> cutEntered = new CompletableFuture<>();
            CompletableFuture<Void> releaseCut = new CompletableFuture<>();
            relocations.gateNextInternalPut(cutEntered, releaseCut);
            var sourceCommit = prepared.commitSourceBarrier(published, NEVER)
                .toCompletableFuture();
            cutEntered.get(3, TimeUnit.SECONDS);
            byte[] lateRecord = ZLinkAcceptedJournalTestRecords.spot(
                SPOT_ID,
                SPOT_ID,
                0,
                "post-freeze-late",
                Map.of(),
                new byte[] {9});
            accepted.add(context.enqueueAcceptedDispatch(
                    lateRecord,
                    () -> fail("late source ingress must remain held"),
                    released::incrementAndGet)
                .toCompletableFuture());
            releaseCut.complete(null);
            var activated = sourceCommit.get(3, TimeUnit.SECONDS);
            assertTrue(accepted.stream().noneMatch(CompletableFuture::isDone));
            assertEquals(0, released.get());

            var restartedCoordinator =
                new ZLinkAggregateRelocationCoordinator(
                    repository, relocations);
            var durable = restartedCoordinator.readPublishedAggregate(
                    finalPrepared.request().participants().stream()
                        .map(value -> new ZLinkAggregateRelocationCoordinator
                            .ExpectedParticipant(
                                value.authorityKey(),
                                value.objectGeneration(),
                                value.authorityOwnerGeneration()))
                        .toList(),
                    finalPrepared.fence(),
                    finalPrepared.request().targetOwner(),
                    finalPrepared.inventoryDigest(),
                    NEVER)
                .toCompletableFuture().get();
            var decoded = ZLinkCanonicalUserSpotRelocationEnvelope.decode(
                durable.payload(),
                TARGET_RID,
                ignored -> LiveSpot.class,
                prepared.stageRequest());
            assertEquals(1026, decoded.acceptedJournal().get("spot").size());

            // The publish ACK is the terminal ownership boundary for the
            // retained source resources.
            prepared.completeSourceBarrierCommit();
            CompletableFuture.allOf(
                    accepted.toArray(CompletableFuture[]::new))
                .get(3, TimeUnit.SECONDS);
            assertEquals(1026, released.get());
            prepared.cleanupLocal(Instant.now().plusSeconds(5))
                .toCompletableFuture().get();
            prepared.completeSourceCleanup(
                    activated,
                    prepared.stagedRoot().request().root(),
                    NEVER)
                .toCompletableFuture().get();
            prepared.discardInitialAfterCommit().toCompletableFuture().get();
            prepared.releasePermitAfterCompletion();
            assertEquals(0, permits.snapshot().outboundUnits());
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
    void boundSessionUsesCommand43HighWaterAndAbortReleasesTheExactSeal()
        throws Exception {
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        ZLinkLocationRepository baseRepository =
            new ZLinkProviderLocationRepository(locations);
        AtomicInteger authorityAbortAttempts = new AtomicInteger();
        ZLinkLocationRepository repository = uncertainPreparationRepository(
            baseRepository,
            authorityAbortAttempts);
        InMemoryRelocationStore relocations = new InMemoryRelocationStore();
        DefaultZLinkFrameworkOptions options = options(locations, relocations);
        var registration = options.registration();
        var nodeRegistration = registration.meshNodes().getFirst();
        var codec = new ZLinkServiceM6BWireCodec();
        List<ZLinkServiceM6BWireCodec.SessionRelocationSeal> seals =
            new CopyOnWriteArrayList<>();
        List<ZLinkServiceM6BWireCodec.SessionRelocationRoute> aborts =
            new CopyOnWriteArrayList<>();
        AtomicBoolean rejectAbortAcks = new AtomicBoolean(true);
        RoutingId sessionRid = RoutingId.from("session-a");
        RoutingId secondSessionRid = RoutingId.from("session-b");
        try (ZLinkFrameworkRuntime host =
                ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkSpotRuntime runtime = (ZLinkSpotRuntime) host.spotManager();
            ZLinkActorRuntime actorRuntime =
                (ZLinkActorRuntime) host.actorManager();
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
            host.actorManager().create(ACTOR_ID, ACTOR_TYPE)
                .submit().toCompletableFuture().get();
            TestActor actor = TestActor.last.get();
            assertNotNull(actor);
            host.actorManager().create(SECOND_ACTOR_ID, ACTOR_TYPE)
                .submit().toCompletableFuture().get();
            TestActor secondActor = TestActor.last.get();
            assertNotNull(secondActor);
            ZLinkBackendActorRef actorRef = actorRuntime.currentRef(actor);
            actorRuntime.commitJoinedLocation(actor, actorRef, SPOT_ID)
                .toCompletableFuture().get();
            actorRuntime.markJoined(
                    actor,
                    actorRef,
                    SPOT_ID,
                    LiveSpot.last.get())
                .toCompletableFuture().get();
            ZLinkBackendActorRef secondActorRef =
                actorRuntime.currentRef(secondActor);
            actorRuntime.commitJoinedLocation(
                    secondActor,
                    secondActorRef,
                    SPOT_ID)
                .toCompletableFuture().get();
            actorRuntime.markJoined(
                    secondActor,
                    secondActorRef,
                    SPOT_ID,
                    LiveSpot.last.get())
                .toCompletableFuture().get();
            assertEquals(List.of(ACTOR_ID, SECOND_ACTOR_ID),
                runtime.actorSessions().actorIdsInSpot(SPOT_ID).stream()
                    .sorted().toList());

            actorRuntime.bindNativeSession(
                actor,
                inertSpotNode(),
                actorRef,
                source.rid(),
                sessionRid,
                12,
                999);
            actorRuntime.bindNativeSession(
                secondActor,
                inertSpotNode(),
                secondActorRef,
                source.rid(),
                secondSessionRid,
                13,
                777);
            repository.updateMeshNode(
                    descriptor(TARGET_RID, 9, targetOwner,
                        "inproc://retire-target"),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();

            ZLinkInternalMeshNode peer = relocationPeer(
                codec,
                seals,
                aborts,
                Map.of(sessionRid, 41L, secondSessionRid, 73L),
                rejectAbortAcks);
            ZLinkAggregateRelocationCoordinator coordinator =
                new ZLinkAggregateRelocationCoordinator(
                    repository, relocations);
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
                    nodeRegistration.relocatableActorFactories(),
                    runtime,
                    new ZLinkSessionRelocationPeerClient(peer));

            ZLinkUserSpotRetireSourceBuilder.PreparedSource prepared =
                builder.prepare(
                    SPOT_ID,
                    rollingToVersionOne(),
                    NEVER).toCompletableFuture().get();

            assertEquals(2, seals.size());
            assertTrue(seals.stream().allMatch(seal ->
                seal.senderRole()
                    == ZLinkServiceM6BWireCodec.RelocationRole.SOURCE));
            assertTrue(seals.stream().allMatch(seal ->
                seal.actor().actor().nodeRid().equals(source.rid())
                    && seal.actor().targetNodeGeneration()
                        == source.lifecycleGeneration()));
            assertEquals(
                Map.of(ACTOR_ID, 41L, SECOND_ACTOR_ID, 73L),
                prepared.stageRequest().sessionRoutes().stream()
                    .collect(java.util.stream.Collectors.toMap(
                        ZLinkSpotRetireControl.SessionRouteFence::actorId,
                        ZLinkSpotRetireControl.SessionRouteFence
                            ::lastAcceptedSessionSequence)),
                "every staged fence must use command 43, not local estimates");

            assertThrows(CompletionException.class, () ->
                prepared.freezeAndPrepareFinal(NEVER)
                    .toCompletableFuture().join());
            assertEquals(1, authorityAbortAttempts.get(),
                "the coordinator first tries to reconcile the lost prepare");
            assertTrue(aborts.isEmpty(),
                "Session abort must wait for an Aborted authority result");

            assertThrows(CompletionException.class, () ->
                prepared.abortPrecommit().toCompletableFuture().join());
            assertEquals(2, authorityAbortAttempts.get(),
                "the exact uncertain fence must reach Aborted before command 44");
            assertEquals(1, builder.unresolvedPreparationCount());
            assertEquals(1, permits.snapshot().outboundUnits(),
                "a missing command 45 must keep the source permit and seal");
            assertSame(LiveSpot.last.get(), runtime.spotFor(SPOT_ID),
                "the source aggregate remains installed while ACK is pending");

            rejectAbortAcks.set(false);
            builder.reconcileUnresolvedPreparations()
                .toCompletableFuture().get();
            assertEquals(0, builder.unresolvedPreparationCount());

            Map<RoutingId, Long> abortCounts = aborts.stream().collect(
                java.util.stream.Collectors.groupingBy(
                    abort -> abort.session().sessionRid(),
                    java.util.stream.Collectors.counting()));
            assertEquals(
                Map.of(sessionRid, 0L, secondSessionRid, 0L).keySet(),
                abortCounts.keySet());
            assertTrue(abortCounts.values().stream().allMatch(count -> count >= 2),
                "both exact aborts follow the retry schedule and converge");
            for (var abort : aborts) {
                var exactSeal = seals.stream()
                    .filter(seal -> seal.session().equals(abort.session()))
                    .findFirst().orElseThrow();
                assertEquals(exactSeal.relocation(), abort.relocation());
                assertEquals(exactSeal.coordinator(), abort.coordinator());
                assertEquals(ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
                    abort.senderRole());
                assertEquals(
                    ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
                    abort.action());
                assertEquals(0, abort.lastAcceptedSessionSequence(),
                    "abort command 44 carries no high-water body");
            }
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
        mesh.objects().server().addActorFactory(
            ACTOR_TYPE,
            TestActor.class,
            TestActorFactory.class,
            factory -> factory.recreateOnRelocation());
        options.validate();
        return options;
    }

    private static ZLinkInternalSpotNode inertSpotNode() {
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkUserSpotRetireSourceBuilderTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("toString")) {
                    return "session-owner";
                }
                throw new UnsupportedOperationException(method.getName());
            });
    }

    private static ZLinkInternalMeshNode relocationPeer(
        ZLinkServiceM6BWireCodec codec,
        List<ZLinkServiceM6BWireCodec.SessionRelocationSeal> seals,
        List<ZLinkServiceM6BWireCodec.SessionRelocationRoute> aborts,
        Map<RoutingId, Long> acceptedHighWaters,
        AtomicBoolean rejectAbortAcks) {
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkUserSpotRetireSourceBuilderTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("requestSessionRelocationSeal")) {
                    var command = codec.decodeSessionRelocationSeal(
                        (byte[]) arguments[1]);
                    seals.add(command);
                    long acceptedHighWater = acceptedHighWaters.get(
                        command.session().sessionRid());
                    return CompletableFuture.completedFuture(
                        codec.encodeSessionRelocationSealed(
                            new ZLinkServiceM6BWireCodec
                                .SessionRelocationSealed(
                                    command.relocation(),
                                    command.coordinator(),
                                    command.actor(),
                                    command.session(),
                                    acceptedHighWater)));
                }
                if (method.getName().equals("requestSessionRelocationRoute")) {
                    var command = codec.decodeSessionRelocationRoute(
                        (byte[]) arguments[1]);
                    aborts.add(command);
                    long acceptedHighWater = acceptedHighWaters.get(
                        command.session().sessionRid());
                    long acknowledgedHighWater = rejectAbortAcks.get()
                        ? acceptedHighWater + 1
                        : acceptedHighWater;
                    return CompletableFuture.completedFuture(
                        codec.encodeSessionRelocationRouted(
                            new ZLinkServiceM6BWireCodec
                                .SessionRelocationRouted(
                                    command.relocation(),
                                    command.coordinator(),
                                    command.actor(),
                                    command.session(),
                                    command.action(),
                                    ZLinkServiceM6BWireCodec
                                        .SessionRelocationRouteResult.APPLIED,
                                    command.currentAuthorityOwnerGeneration(),
                                    acknowledgedHighWater)));
                }
                if (method.getName().equals("toString")) {
                    return "session-relocation-peer";
                }
                throw new UnsupportedOperationException(method.getName());
            });
    }

    private static ZLinkLocationRepository uncertainPreparationRepository(
        ZLinkLocationRepository delegate,
        AtomicInteger abortAttempts) {
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkUserSpotRetireSourceBuilderTest.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("prepareAggregate")) {
                    @SuppressWarnings("unchecked")
                    CompletionStage<Object> prepared =
                        (CompletionStage<Object>) invoke(
                            delegate, method, arguments);
                    return prepared.thenCompose(ignored ->
                        CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "aggregate prepare response was lost")));
                }
                if (method.getName().equals("abortAggregate")
                    && abortAttempts.incrementAndGet() == 1) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "first abort response was unavailable"));
                }
                return invoke(delegate, method, arguments);
            });
    }

    private static Object invoke(
        Object target,
        java.lang.reflect.Method method,
        Object[] arguments) throws Throwable {
        try {
            return method.invoke(target, arguments);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
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
            List.of(
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    STABLE_TYPE,
                    ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                    true,
                    0),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.ACTOR,
                    ACTOR_TYPE,
                    ZLinkObjectMaintenancePolicyKind.RECREATE,
                    false,
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

    public static final class TestActor implements ZLinkActor {
        private static final AtomicReference<TestActor> last =
            new AtomicReference<>();
        private final ZLinkActorContext context;

        public TestActor(ZLinkActorContext context) {
            this.context = context;
            last.set(this);
        }

        @Override public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class TestActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new TestActor(context));
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
