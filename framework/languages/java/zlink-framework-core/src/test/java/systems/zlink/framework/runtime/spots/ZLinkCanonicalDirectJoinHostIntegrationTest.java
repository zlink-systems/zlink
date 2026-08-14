package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;

final class ZLinkCanonicalDirectJoinHostIntegrationTest {
    private static final String MESH = "canonical-direct-join";
    private static final String ACTOR_TYPE = "player";
    private static final String SPOT_TYPE = "room";
    private static final String ACTOR_ID = "actor-direct";
    private static final String TARGET_SPOT_ID = "room-direct";
    private static final RoutingId SOURCE_RID = RoutingId.from("join-source");
    private static final RoutingId TARGET_RID = RoutingId.from("join-target");
    private static final RoutingId SESSION_RID = RoutingId.from("session-a");
    private static final Duration TIMEOUT = Duration.ofSeconds(5);
    private static final systems.zlink.framework.runtime.internal.locations
        .ZLinkStoreCancellation OPEN = () -> false;

    private static final List<String> EVENTS = new CopyOnWriteArrayList<>();
    private static final AtomicReference<CompletableFuture<Void>> LEAVE_GATE =
        new AtomicReference<>();
    private static final AtomicReference<ZLinkActorJoinOperationId> ACCEPTED =
        new AtomicReference<>();
    private static final AtomicReference<CompletableFuture<
        ZLinkActorJoinOperationId>> ACCEPTED_STAGE = new AtomicReference<>();

    @ParameterizedTest(name = "{0}")
    @EnumSource(Scenario.class)
    void actualHostsUseOneCanonicalOwnerForB1B2D1AndLifecycleOrdering(
        Scenario scenario)
        throws Exception {
        EVENTS.clear();
        ACCEPTED.set(null);
        ACCEPTED_STAGE.set(new CompletableFuture<>());
        LEAVE_GATE.set(new CompletableFuture<>());
        var locationStore = new ZLinkInMemoryLocationStore();
        var relocationStore = new InMemoryRelocationStore();
        Duration sourceRetention = scenario == Scenario.NORMAL
            ? Duration.ofMillis(100)
            : Duration.ofSeconds(3);
        var targetOptions = options(
            TARGET_RID, "inproc://canonical-direct-target",
            locationStore, relocationStore, sourceRetention);
        var sourceOptions = options(
            SOURCE_RID, "inproc://canonical-direct-source",
            locationStore, relocationStore, sourceRetention);

        try (ZLinkFrameworkRuntime targetHost =
                 ZLinkFrameworkRuntimeTestAccess.start(targetOptions);
             ZLinkFrameworkRuntime sourceHost =
                 ZLinkFrameworkRuntimeTestAccess.start(sourceOptions)) {
            ZLinkSpotRuntime targetSpots =
                (ZLinkSpotRuntime) targetHost.spotManager();
            ZLinkSpotRuntime sourceSpots =
                (ZLinkSpotRuntime) sourceHost.spotManager();
            ZLinkActorRuntime targetActors =
                (ZLinkActorRuntime) targetHost.actorManager();
            ZLinkActorRuntime sourceActors =
                (ZLinkActorRuntime) sourceHost.actorManager();
            targetHost.spotManager()
                .getOrCreate(TARGET_SPOT_ID, SPOT_TYPE)
                .submit().toCompletableFuture().get();
            Object targetSpot = targetSpots.spotFor(TARGET_SPOT_ID);
            assertInstanceOf(
                ZLinkActorCreateResult.Created.class,
                sourceHost.actorManager().create(ACTOR_ID, ACTOR_TYPE)
                    .submit().toCompletableFuture().get());

            var repository = new ZLinkProviderLocationRepository(locationStore);
            AtomicInteger targetCommits = new AtomicInteger();
            AtomicLong firstCommitNanos = new AtomicLong();
            ZLinkLocationRepository observed = observed(
                repository, targetCommits, firstCommitNanos);
            var coordinator = new ZLinkAggregateRelocationCoordinator(
                observed, relocationStore);
            ZLinkMeshNodeDescriptor sourceDescriptor = descriptor(
                repository, SOURCE_RID);
            ZLinkMeshNodeDescriptor targetDescriptor = descriptor(
                repository, TARGET_RID);
            ZLinkAuthoritySnapshot targetSpotAuthority = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                repository.read(
                    ZLinkAuthorityKeyCodec.spot(TARGET_SPOT_ID), OPEN)
                    .toCompletableFuture().get());
            ZLinkAuthoritySnapshot sourceAuthority = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                repository.read(
                    ZLinkAuthorityKeyCodec.actor(ACTOR_ID), OPEN)
                    .toCompletableFuture().get());

            var sourceRegistration =
                sourceOptions.registration().meshNodes().getFirst();
            var targetRegistration =
                targetOptions.registration().meshNodes().getFirst();
            var sourceAdapters = new ZLinkRelocationAdapterRegistry(
                sourceOptions.registration(), ZLinkHandlerActivator.reflection());
            var targetAdapters = new ZLinkRelocationAdapterRegistry(
                targetOptions.registration(), ZLinkHandlerActivator.reflection());
            verifyReadyFailureDiscardsUserSpotTimers(
                targetActors, targetSpots, targetAdapters, targetSpot);
            EVENTS.clear();

            var link = new CanonicalLink(
                sourceDescriptor,
                targetDescriptor,
                sourceSpots,
                sourceAuthority,
                scenario);
            var sourceSessionClient =
                new ZLinkSessionRelocationPeerClient(link.sourceNode());
            var targetSessionClient =
                new ZLinkSessionRelocationPeerClient(link.targetNode());
            var sourceBuilder = new ZLinkStandaloneActorRelocationSourceBuilder(
                MESH,
                SOURCE_RID,
                sourceDescriptor.lifecycleGeneration(),
                observed,
                coordinator,
                sourceSpots.actorSessions(),
                sourceAdapters,
                sourceRegistration.relocatableActorFactories(),
                sourceSpots,
                sourceSessionClient,
                TIMEOUT);
            var targetBuilder = new ZLinkStandaloneActorRelocationSourceBuilder(
                MESH,
                TARGET_RID,
                targetDescriptor.lifecycleGeneration(),
                observed,
                coordinator,
                targetSpots.actorSessions(),
                targetAdapters,
                targetRegistration.relocatableActorFactories(),
                targetSpots,
                targetSessionClient,
                TIMEOUT);
            var sourceJoin = new ZLinkActorJoinCanonicalAdapter(
                sourceActors, sourceSpots);
            var targetJoin = new ZLinkActorJoinCanonicalAdapter(
                targetActors, targetSpots);

            var targetBackend = new RecordingTargetBackend(
                TARGET_RID,
                targetSpots.actorSessions(),
                targetAdapters,
                targetSpots);
            var endpoint = new ZLinkUserSpotRetireTargetEndpoint(
                TARGET_RID,
                targetDescriptor.lifecycleGeneration(),
                coordinator,
                unusedSpotStaging(),
                ignored -> null,
                (lane, queued) -> {
                    assertEquals("actor:" + ACTOR_ID, lane);
                    assertTrue(targetBackend.open.get(),
                        "regular replay must begin only after admission opens");
                    EVENTS.add("replay:" + ZLinkActorAcceptedJournal.decode(
                        queued.payload()).header().packetName());
                    return CompletableFuture.completedFuture(null);
                },
                targetSessionClient,
                TIMEOUT,
                request -> {
                    EVENTS.add("target.normalize");
                    return coordinator.normalizePublishedAggregate(
                        request.participants().stream()
                            .map(value -> new ZLinkAggregateRelocationCoordinator
                                .ExpectedParticipant(
                                    value.authorityKey(),
                                    value.objectGeneration(),
                                    value.sourceAuthorityOwnerGeneration()))
                            .toList(),
                        new ZLinkAggregateFence(
                            request.fence().aggregateId(),
                            request.fence().aggregateGeneration()),
                        new ZLinkLocationOwnerToken(
                            request.targetOwnerId(),
                            request.targetOwnerLeaseGeneration()),
                        OPEN).whenComplete((ignored, failure) -> {
                            if (failure == null) {
                                link.targetCompletion.complete(null);
                            } else {
                                link.targetCompletion.completeExceptionally(
                                    failure);
                            }
                        });
                },
                null,
                null,
                observed,
                new ZLinkStandaloneActorRelocationStagingOwner(targetBackend),
                targetJoin);
            link.endpoint.set(endpoint);

            var sourceMachine = new ZLinkCanonicalRelocationStateMachine(
                link.sourceNode(),
                MESH,
                sourceRegistration.entrySpotId(),
                observed,
                coordinator,
                unusedRelocationTarget());
            ZLinkSpotRetireControl.TargetEndpoint observedEndpoint =
                new ZLinkSpotRetireControl.TargetEndpoint() {
                    @Override public ZLinkSpotRetireControl.TargetProfile
                        applyTargetProfile(
                            ZLinkSpotRetireControl.StageRequest request,
                            long defaultActorSpotGeneration) {
                        try {
                            var profile = endpoint.applyTargetProfile(
                                request, defaultActorSpotGeneration);
                            EVENTS.add("target.profile:" +
                                profile.actorSpotGeneration() + ":" +
                                profile.actorSpotKind());
                            return profile;
                        } catch (RuntimeException failure) {
                            EVENTS.add("target.profile.failed:" + failure);
                            throw failure;
                        }
                    }

                    @Override public CompletionStage<Void> stage(
                        ZLinkSpotRetireControl.StageRequest request) {
                        link.targetRequest.set(request);
                        EVENTS.add("target.stage.enter");
                        return endpoint.stage(request).whenComplete(
                            (ignored, failure) -> EVENTS.add(
                                failure == null
                                    ? "target.stage.done"
                                    : "target.stage.failed:"
                                        + failure.getClass().getSimpleName()));
                    }

                    @Override public CompletionStage<Void> publish(
                        ZLinkSpotRetireControl.StageRequest request) {
                        return endpoint.publish(request);
                    }

                    @Override public CompletionStage<Void> stageRelayedRecord(
                        ZLinkSpotRetireControl.StageRequest request,
                        byte[] frozenRecord) {
                        return endpoint.stageRelayedRecord(
                            request, frozenRecord);
                    }

                    @Override public CompletionStage<Void> abort(
                        ZLinkSpotRetireControl.StageRequest request) {
                        return endpoint.abort(request);
                    }
                };
            var targetMachine = new ZLinkCanonicalRelocationStateMachine(
                link.targetNode(),
                MESH,
                targetRegistration.entrySpotId(),
                observed,
                coordinator,
                observedEndpoint);
            link.sourceMachine.set(sourceMachine);
            link.targetMachine.set(targetMachine);
            sourceJoin.register(
                SOURCE_RID, link.sourceNode(), sourceBuilder, sourceMachine);
            targetJoin.register(
                TARGET_RID, link.targetNode(), targetBuilder, targetMachine);

            ZLinkActor sourceActor = sourceActors.localActor(ACTOR_ID)
                .orElseThrow();
            ZLinkBackendActorRef sourceRef = sourceActors.currentRef(sourceActor);
            sourceActors.bindNativeSession(
                sourceActor,
                inertSpotNode(),
                sourceRef,
                SOURCE_RID,
                SESSION_RID,
                7,
                0);

            CompletableFuture<Void> activeTurn = new CompletableFuture<>();
            ZLinkAsyncSerialQueue sourceQueue =
                sourceSpots.actorSessions().actorRelocationLane(ACTOR_ID);
            CompletionStage<Void> blocker = sourceQueue.enqueueLifecycleBarrier(
                () -> activeTurn);
            AtomicInteger released = new AtomicInteger();
            CompletionStage<Void> b1Accepted = sourceQueue.enqueueRelocatable(
                actorRecord("B1"),
                () -> fail("B1 must transfer instead of executing at source"),
                released::incrementAndGet);

            UUID relocationId = UUID.randomUUID();
            ZLinkActorJoinOperationId operationId =
                new ZLinkActorJoinOperationId(0x1111L, 0x2222L);
            assertNotEquals(
                relocationId,
                new UUID(operationId.high(), operationId.low()));
            var admission = new ZLinkActorJoinRelocationPort.Admission(
                relocationId,
                operationId,
                ACTOR_ID,
                TARGET_SPOT_ID,
                targetSpot,
                actor -> ((TrackingSpot) targetSpot).onJoinedActor(actor),
                ZLinkMessage.of("accepted"),
                TIMEOUT);
            targetJoin.admit(admission);
            var goal = new ZLinkActorJoinRelocationPort.Goal(
                relocationId,
                operationId,
                sourceRef,
                ACTOR_TYPE,
                TARGET_SPOT_ID,
                targetSpotAuthority.objectGeneration(),
                TARGET_RID,
                targetDescriptor.lifecycleGeneration(),
                targetSpotAuthority.authorityOwnerGeneration(),
                targetDescriptor.leaseGeneration(),
                new byte[] {9, 4});

            link.readyHook.set(() -> {
                CompletionStage<Void> b2 = sourceQueue.enqueueRelocatable(
                    actorRecord("B2"),
                    () -> fail(
                        "B2 must transfer instead of executing at source"),
                    released::incrementAndGet);
                link.b2Accepted.set(b2);
                injectD1(
                    endpoint,
                    sourceDescriptor,
                    targetDescriptor,
                    sourceAuthority);
            });
            CompletableFuture<ZLinkActorJoinRelocationPort.Submission> moved =
                sourceJoin.relocate(goal, TIMEOUT).toCompletableFuture();
            assertFalse(moved.isDone(),
                "the source turn boundary must own capture admission");
            activeTurn.complete(null);
            blocker.toCompletableFuture().get(1, TimeUnit.SECONDS);

            ZLinkActorJoinRelocationPort.Submission submission;
            try {
                submission = moved.get(5, TimeUnit.SECONDS);
            } catch (java.util.concurrent.TimeoutException timeout) {
                fail("canonical Join stalled: " + EVENTS, timeout);
                return;
            }
            assertEquals(TARGET_RID, submission.targetActor().nodeRid());
            assertEquals(operationId,
                ACCEPTED_STAGE.get().get(4, TimeUnit.SECONDS));
            assertEquals(operationId, ACCEPTED.get());
            assertFalse(LEAVE_GATE.get().isDone(),
                "source OnLeave completion must not gate target Accepted");
            if (scenario.leaveMode() == LeaveMode.DELIVER) {
                assertNotNull(link.leftCompletion.get(),
                    "command 29 must start the source handler without an ACK");
            } else {
                assertNull(link.leftCompletion.get(),
                    "source handler execution must not be required for Accepted");
                assertFalse(EVENTS.contains("leave.handler"));
            }
            assertEquals(1, targetCommits.get(),
                "only the target canonical owner may commit authority");
            if (scenario.dropCutover()) {
                assertTrue(link.readyTransportSuccessNanos.get() > 0L);
                assertTrue(firstCommitNanos.get()
                        - link.readyTransportSuccessNanos.get()
                        >= TimeUnit.SECONDS.toNanos(1),
                    "fallback may publish only after the exact 1000ms delay");
            }
            link.targetCompletion.get(4, TimeUnit.SECONDS);
            assertEquals(
                List.of("B1", "B2", "D1"),
                EVENTS.stream()
                    .filter(value -> value.startsWith("replay:"))
                    .map(value -> value.substring("replay:".length()))
                    .toList());
            assertBefore("target.joined", "leave.submit");
            assertBefore("leave.submit", "target.accepted");
            assertBefore("target.accepted", "target.open");
            assertBefore("target.open", "replay:B1");
            assertBefore("replay:D1", "command44");
            assertBefore("command44", "target.normalize");
            assertTrue(EVENTS.contains("route.duplicate.noop"));
            assertTrue(EVENTS.contains("route.changed.conflict"));
            assertEquals(2, released.get());
            b1Accepted.toCompletableFuture().get(1, TimeUnit.SECONDS);
            link.b2Accepted.get().toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

            byte[] cutover = link.cutover.get();
            assertNotNull(cutover);
            targetMachine.apply(
                    SOURCE_RID,
                    ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
                    cutover)
                .toCompletableFuture().get(1, TimeUnit.SECONDS);
            targetMachine.apply(
                    SOURCE_RID,
                    ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
                    cutover)
                .toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertEquals(1, targetCommits.get(),
                "late and duplicate CUTOVER must mutate nothing");

            if (scenario.leaveMode() != LeaveMode.DELIVER) {
                link.submitDuplicateLeft();
            }
            link.submitDuplicateLeft();
            assertEquals(2, EVENTS.stream()
                .filter("leave.handler"::equals).count(),
                "the duplicate command 29 reaches the handler and is a no-op");
            if (scenario == Scenario.NORMAL) {
                TimeUnit.MILLISECONDS.sleep(250);
                assertEquals(0, sourceAttemptCount(sourceJoin),
                    "Message Follow retention must bound the source attempt even "
                        + "while OnLeave remains pending");
            }
            assertEquals(1, EVENTS.stream()
                .filter("source.leave"::equals).count(),
                "retention cleanup must not duplicate claimed OnLeave");
            LEAVE_GATE.get().complete(null);
            link.leftCompletion.get().toCompletableFuture()
                .get(3, TimeUnit.SECONDS);
            assertEquals(1, EVENTS.stream()
                .filter("source.leave"::equals).count());
            assertTrue(sourceActors.localActor(ACTOR_ID).isEmpty());
            assertEquals(0, sourceAttemptCount(sourceJoin));
        } finally {
            LEAVE_GATE.get().complete(null);
        }
    }

    private static DefaultZLinkFrameworkOptions options(
        RoutingId rid,
        String endpoint,
        ZLinkInMemoryLocationStore locations,
        InMemoryRelocationStore relocations,
        Duration sourceRetention) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        setMessageFollowDuration(options, sourceRetention);
        var mesh = options.addRouteMesh(MESH)
            .setRoutingId(rid)
            .listen(endpoint);
        mesh.channelName(MESH).server();
        mesh.objects().server()
            .addEntrySpot(TrackingEntrySpot.class)
            .addSpotFactory(
                SPOT_TYPE,
                TrackingSpot.class,
                factory -> factory.disableRelocation())
            .addActorFactory(
                ACTOR_TYPE,
                TrackingActor.class,
                TrackingActorFactory.class,
                factory -> factory.preserveStateWith(SnapshotAdapter.class));
        options.validate();
        return options;
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        ZLinkLocationRepository repository,
        RoutingId rid) {
        return repository.listMeshNodes(MESH, ZLinkPageRequest.firstPage())
            .toCompletableFuture().join().items().stream()
            .filter(value -> value.rid().equals(rid))
            .findFirst().orElseThrow();
    }

    private static ZLinkLocationRepository observed(
        ZLinkLocationRepository delegate,
        AtomicInteger commits,
        AtomicLong firstCommitNanos) {
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkLocationRepository.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("commitAggregate")) {
                    commits.incrementAndGet();
                    firstCommitNanos.compareAndSet(0L, System.nanoTime());
                } else if (method.getName().equals("prepareAggregate")) {
                    EVENTS.add("authority.prepare");
                }
                try {
                    return method.invoke(delegate, arguments);
                } catch (InvocationTargetException failure) {
                    throw failure.getCause();
                }
            });
    }

    private static byte[] actorRecord(String packetName) {
        return ZLinkAcceptedJournalTestRecords.actor(
            ACTOR_ID, 0, packetName, Map.of(), new byte[] {1});
    }

    private static void injectD1(
        ZLinkUserSpotRetireTargetEndpoint endpoint,
        ZLinkMeshNodeDescriptor source,
        ZLinkMeshNodeDescriptor target,
        ZLinkAuthoritySnapshot sourceAuthority) {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        assertTrue(endpoint.handleActor(
            new ZLinkInternalMeshNode.PeerAuthorityFence(
                source.rid(),
                source.lifecycleGeneration(),
                source.ownerId(),
                source.leaseGeneration()),
            new ZLinkServiceM6BWireCodec.ActorMessage(
                false,
                0,
                null,
                0,
                0,
                1,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    new ZLinkBackendActorRef(
                        target.rid(),
                        ACTOR_ID,
                        sourceAuthority.objectGeneration()),
                    target.lifecycleGeneration(),
                    sourceAuthority.authorityOwnerGeneration() + 1,
                    target.leaseGeneration())),
            () -> actorRecord("D1"),
            List.of(Message.from("d1")),
            null,
            ignored -> fail("D1 send must not produce a reply"),
            failure::set));
        assertNull(failure.get());
        EVENTS.add("target.d1.queued");
    }

    private static void verifyReadyFailureDiscardsUserSpotTimers(
        ZLinkActorRuntime targetActors,
        ZLinkSpotRuntime targetSpots,
        ZLinkRelocationAdapterRegistry targetAdapters,
        Object targetSpot) throws Exception {
        String actorId = "ready-failure-timer-probe";
        UUID relocationId = UUID.randomUUID();
        var request = new ZLinkStandaloneActorRelocationStagingOwner.Request(
            relocationId, actorId, ACTOR_TYPE, 1, 1, true, TARGET_SPOT_ID);
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(
            TARGET_RID,
            targetSpots.actorSessions(),
            targetAdapters,
            targetSpots);
        var staged = owner.stage(
                request,
                ZLinkCanonicalActorRelocationEnvelope.encode(
                    relocationId,
                    actorId,
                    1,
                    1,
                    true,
                    new byte[] {7, 2, 6},
                    List.of()))
            .toCompletableFuture().get(1, TimeUnit.SECONDS);
        assertTrue(hasActorTimerOwner(targetSpot, actorId));

        owner.discard(staged).toCompletableFuture()
            .get(1, TimeUnit.SECONDS);

        assertFalse(hasActorTimerOwner(targetSpot, actorId),
            "READY rollback must discard the staged User Spot timer owner");
        assertTrue(targetActors.localActor(actorId).isEmpty());
    }

    private static boolean hasActorTimerOwner(
        Object spot,
        String actorId) throws Exception {
        Object context = ((ZLinkSpot<?>) spot).context();
        Field field = context.getClass().getDeclaredField("actorTimers");
        field.setAccessible(true);
        return ((Map<?, ?>) field.get(context)).containsKey(actorId);
    }

    private static ZLinkUserSpotAggregateStagingOwner unusedSpotStaging() {
        return new ZLinkUserSpotAggregateStagingOwner(
            new ZLinkUserSpotAggregateStagingOwner.StagingBackend() {
                private <T> CompletionStage<T> unused() {
                    return CompletableFuture.failedFuture(
                        new AssertionError("Spot aggregate path is unused"));
                }
                @Override public CompletionStage<Object> prepareSpot(
                    ZLinkUserSpotAggregateStagingOwner.Request request) {
                    return unused();
                }
                @Override public CompletionStage<Void> restoreSpot(
                    Object spot,
                    ZLinkUserSpotAggregateStagingOwner.Request request,
                    ZLinkRelocationCancellation cancellation) { return unused(); }
                @Override public CompletionStage<Object> prepareActor(
                    ZLinkUserSpotAggregateStagingOwner.ActorParticipant actor,
                    ZLinkRelocationCancellation cancellation) { return unused(); }
                @Override public void publishSpot(Object spot) { fail(); }
                @Override public void publishActor(Object actor) { fail(); }
                @Override public void completeActor(Object actor) { fail(); }
                @Override public void publishTimers(Object spot) { fail(); }
                @Override public CompletionStage<Void> discardActor(Object actor) {
                    return unused();
                }
                @Override public void discardSpot(Object spot) { fail(); }
            });
    }

    private static ZLinkSpotRetireControl.TargetEndpoint
        unusedRelocationTarget() {
        return new ZLinkSpotRetireControl.TargetEndpoint() {
            private CompletionStage<Void> unused() {
                return CompletableFuture.failedFuture(
                    new AssertionError("source must not act as target"));
            }
            @Override public CompletionStage<Void> stage(
                ZLinkSpotRetireControl.StageRequest request) { return unused(); }
            @Override public CompletionStage<Void> publish(
                ZLinkSpotRetireControl.StageRequest request) { return unused(); }
            @Override public CompletionStage<Void> abort(
                ZLinkSpotRetireControl.StageRequest request) { return unused(); }
        };
    }

    private static ZLinkInternalSpotNode inertSpotNode() {
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> defaultValue(method.getReturnType()));
    }

    private static void assertBefore(String first, String second) {
        int left = EVENTS.indexOf(first);
        int right = EVENTS.indexOf(second);
        assertTrue(left >= 0, () -> first + " missing: " + EVENTS);
        assertTrue(right >= 0, () -> second + " missing: " + EVENTS);
        assertTrue(left < right,
            () -> first + " must precede " + second + ": " + EVENTS);
    }

    private static int sourceAttemptCount(
        ZLinkActorJoinCanonicalAdapter adapter) throws Exception {
        Field field = ZLinkActorJoinCanonicalAdapter.class
            .getDeclaredField("sources");
        field.setAccessible(true);
        return ((Map<?, ?>) field.get(adapter)).size();
    }

    private static void setMessageFollowDuration(
        DefaultZLinkFrameworkOptions options,
        Duration retention) {
        try {
            Field field = options.registration().getClass()
                .getDeclaredField("messageFollowDuration");
            field.setAccessible(true);
            field.set(options.registration(), retention);
        } catch (ReflectiveOperationException failure) {
            throw new AssertionError(failure);
        }
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) return null;
        if (type == boolean.class) return false;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0F;
        if (type == double.class) return 0D;
        if (type == char.class) return '\0';
        return null;
    }

    private enum Scenario {
        NORMAL(false, LeaveMode.DELIVER),
        FALLBACK_LEAVE_FAILED_FUTURE(true, LeaveMode.FAILED_FUTURE),
        FALLBACK_LEAVE_THROW(true, LeaveMode.THROW);

        private final boolean dropCutover;
        private final LeaveMode leaveMode;

        Scenario(boolean dropCutover, LeaveMode leaveMode) {
            this.dropCutover = dropCutover;
            this.leaveMode = leaveMode;
        }

        boolean dropCutover() {
            return dropCutover;
        }

        LeaveMode leaveMode() {
            return leaveMode;
        }
    }

    private enum LeaveMode {
        DELIVER,
        FAILED_FUTURE,
        THROW
    }

    private static final class CanonicalLink {
        private final AtomicReference<ZLinkCanonicalRelocationStateMachine>
            sourceMachine = new AtomicReference<>();
        private final AtomicReference<ZLinkCanonicalRelocationStateMachine>
            targetMachine = new AtomicReference<>();
        private final AtomicReference<ZLinkUserSpotRetireTargetEndpoint>
            endpoint = new AtomicReference<>();
        private final AtomicReference<Runnable> readyHook =
            new AtomicReference<>();
        private final AtomicReference<CompletionStage<Void>> b2Accepted =
            new AtomicReference<>();
        private final AtomicReference<Object> sourceLeftHandler =
            new AtomicReference<>();
        private final AtomicReference<CompletionStage<Void>> leftCompletion =
            new AtomicReference<>();
        private final AtomicReference<byte[]> cutover = new AtomicReference<>();
        private final AtomicReference<ZLinkSpotRetireControl.StageRequest>
            targetRequest = new AtomicReference<>();
        private final AtomicLong readyTransportSuccessNanos = new AtomicLong();
        private final CompletableFuture<Void> targetCompletion =
            new CompletableFuture<>();
        private final AtomicBoolean readyInjected = new AtomicBoolean();
        private final AtomicBoolean routeDuplicateChecked =
            new AtomicBoolean();
        private final ZLinkServiceM6BWireCodec codec =
            new ZLinkServiceM6BWireCodec();
        private final ZLinkInternalMeshNode source;
        private final ZLinkInternalMeshNode target;
        private final ZLinkMeshNodeDescriptor sourceDescriptor;
        private final ZLinkAuthoritySnapshot sourceAuthority;
        private final Scenario scenario;
        private volatile ZLinkServiceM6BWireCodec.ActorLeft lastLeft;

        private CanonicalLink(
            ZLinkMeshNodeDescriptor sourceDescriptor,
            ZLinkMeshNodeDescriptor targetDescriptor,
            ZLinkSpotRuntime sourceSpots,
            ZLinkAuthoritySnapshot sourceAuthority,
            Scenario scenario) {
            this.sourceDescriptor = sourceDescriptor;
            this.sourceAuthority = sourceAuthority;
            this.scenario = scenario;
            source = node(sourceDescriptor, targetMachine, true);
            target = node(targetDescriptor, sourceMachine, false);
        }

        private ZLinkInternalMeshNode sourceNode() { return source; }
        private ZLinkInternalMeshNode targetNode() { return target; }

        private ZLinkInternalMeshNode node(
            ZLinkMeshNodeDescriptor descriptor,
            AtomicReference<ZLinkCanonicalRelocationStateMachine> peer,
            boolean sourceSide) {
            var status = new systems.zlink.framework.runtime.internal.binding.spot
                .MeshNodeStatus(
                    systems.zlink.framework.runtime.internal.binding.spot
                        .MeshNodeState.READY,
                    descriptor.rid(),
                    MESH,
                    "",
                    descriptor.lifecycleGeneration(),
                    1,
                    0,
                    1,
                    1,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0);
            return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
                ZLinkInternalMeshNode.class.getClassLoader(),
                new Class<?>[] {ZLinkInternalMeshNode.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "status" -> status;
                    case "setActorLeftHandler" -> {
                        if (sourceSide) {
                            sourceLeftHandler.set(arguments[0]);
                        }
                        yield null;
                    }
                    case "sendCanonicalRelocationControl" -> {
                        byte[] command = ((byte[]) arguments[1]).clone();
                        int code = Byte.toUnsignedInt(command[3]);
                        if (!sourceSide
                            && code == ServiceWireConstants
                                .COMMAND_RELOCATION_READY
                            && readyInjected.compareAndSet(false, true)) {
                            EVENTS.add("canonical.ready");
                            readyHook.get().run();
                        }
                        if (sourceSide
                            && code == ServiceWireConstants
                                .COMMAND_RELOCATION_PREPARE) {
                            EVENTS.add("canonical.prepare");
                        } else if (sourceSide
                            && code == ServiceWireConstants
                                .COMMAND_RELOCATION_DATA) {
                            EVENTS.add("canonical.data");
                        } else if (sourceSide
                            && code == ServiceWireConstants
                                .COMMAND_RELOCATION_CUTOVER) {
                            EVENTS.add("canonical.cutover");
                            cutover.set(command.clone());
                        }
                        CompletionStage<Void> delivered;
                        if (sourceSide
                            && code == ServiceWireConstants
                                .COMMAND_RELOCATION_CUTOVER
                            && scenario.dropCutover()) {
                            EVENTS.add("canonical.cutover.dropped");
                            delivered = CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "injected CUTOVER submission failure"));
                        } else {
                            delivered = peer.get().apply(
                                descriptor.rid(), code, command);
                        }
                        if (sourceSide
                            && code == ServiceWireConstants
                                .COMMAND_RELOCATION_DATA) {
                            byte[] frozen = ZLinkCanonicalRelocationProtocol
                                .decodeData(command).frozenRecord();
                            if (frozen.length >= 4
                                && Byte.toUnsignedInt(frozen[0])
                                    == ServiceWireConstants.MAGIC_0
                                && Byte.toUnsignedInt(frozen[1])
                                    == ServiceWireConstants.MAGIC_1
                                && Byte.toUnsignedInt(frozen[2])
                                    == ServiceWireConstants.WIRE_MAJOR
                                && Byte.toUnsignedInt(frozen[3])
                                    == ServiceWireConstants
                                        .COMMAND_SESSION_RELOCATION_SEALED
                                && routeDuplicateChecked.compareAndSet(
                                    false, true)) {
                                var sealed = codec
                                    .decodeSessionRelocationSealed(frozen);
                                var session = sealed.session();
                                byte[] changed = codec
                                    .encodeSessionRelocationSealed(
                                        new ZLinkServiceM6BWireCodec
                                            .SessionRelocationSealed(
                                                sealed.relocation(),
                                                sealed.coordinator(),
                                                sealed.actor(),
                                                new ZLinkServiceM6BWireCodec
                                                    .SessionOwnerFence(
                                                        session.nodeRid(),
                                                        session.nodeGeneration(),
                                                        session.ownerId(),
                                                        session
                                                            .ownerLeaseGeneration(),
                                                        session.sessionRid(),
                                                        session
                                                            .bindingGeneration()
                                                            + 1)));
                                delivered = delivered.thenCompose(ignored ->
                                        endpoint.get().stageRelayedRecord(
                                            targetRequest.get(), frozen))
                                    .thenRun(() -> EVENTS.add(
                                        "route.duplicate.noop"))
                                    .thenCompose(ignored -> endpoint.get()
                                        .stageRelayedRecord(
                                            targetRequest.get(), changed))
                                    .handle((ignored, failure) -> {
                                        assertNotNull(failure,
                                            "changed command43 must conflict");
                                        EVENTS.add("route.changed.conflict");
                                        return null;
                                    });
                            }
                        }
                        if (!sourceSide
                            && code == ServiceWireConstants
                                .COMMAND_RELOCATION_READY) {
                            delivered = delivered.whenComplete(
                                (ignored, failure) -> {
                                    if (failure == null) {
                                        readyTransportSuccessNanos.compareAndSet(
                                            0L, System.nanoTime());
                                    }
                                });
                        }
                        yield delivered;
                    }
                    case "requestSessionRelocationSeal" -> {
                        var seal = codec.decodeSessionRelocationSeal(
                            (byte[]) arguments[1]);
                        EVENTS.add("command42");
                        yield CompletableFuture.completedFuture(
                            codec.encodeSessionRelocationSealed(
                                new ZLinkServiceM6BWireCodec
                                    .SessionRelocationSealed(
                                        seal.relocation(),
                                        seal.coordinator(),
                                        seal.actor(),
                                        seal.session())));
                    }
                    case "sendSessionRelocationRoute" -> {
                        var route = codec.decodeSessionRelocationRoute(
                            (byte[]) arguments[1]);
                        assertEquals(
                            ZLinkServiceM6BWireCodec
                                .SessionRelocationRouteAction.COMMIT,
                            route.action());
                        EVENTS.add("command44");
                        yield CompletableFuture.completedFuture(null);
                    }
                    case "sendActorLeft" -> {
                        EVENTS.add("leave.submit");
                        lastLeft = (ZLinkServiceM6BWireCodec.ActorLeft)
                            arguments[1];
                        yield switch (scenario.leaveMode()) {
                            case DELIVER -> {
                                invokeSourceLeft(lastLeft);
                                yield CompletableFuture.completedFuture(null);
                            }
                            case FAILED_FUTURE ->
                                CompletableFuture.failedFuture(
                                    new IllegalStateException(
                                        "injected source leave failure"));
                            case THROW -> throw new IllegalStateException(
                                "injected synchronous source leave failure");
                        };
                    }
                    case "toString" -> descriptor.rid().toString();
                    default -> defaultValue(method.getReturnType());
                });
        }

        private void submitDuplicateLeft() throws Exception {
            invokeSourceLeft(lastLeft);
        }

        @SuppressWarnings("unchecked")
        private void invokeSourceLeft(
            ZLinkServiceM6BWireCodec.ActorLeft left) throws Exception {
            Object handler = sourceLeftHandler.get();
            assertNotNull(handler);
            Method method = handler.getClass().getDeclaredMethods()[0];
            method.setAccessible(true);
            EVENTS.add("leave.handler");
            CompletionStage<Void> completion = (CompletionStage<Void>)
                method.invoke(handler, TARGET_RID, left);
            leftCompletion.compareAndSet(null, completion);
        }
    }

    private static final class RecordingTargetBackend
        implements ZLinkStandaloneActorRelocationStagingOwner.Backend {
        private final RoutingId targetRid;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkRelocationAdapterRegistry adapters;
        private final ZLinkSpotRuntime spots;
        private final AtomicBoolean open = new AtomicBoolean();

        private RecordingTargetBackend(
            RoutingId targetRid,
            ZLinkActorSessionCoordinator actors,
            ZLinkRelocationAdapterRegistry adapters,
            ZLinkSpotRuntime spots) {
            this.targetRid = targetRid;
            this.actors = actors;
            this.adapters = adapters;
            this.spots = spots;
        }

        @Override public CompletionStage<Object> prepare(
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            EVENTS.add("target.prepare");
            return actors.prepareRelocatedActor(
                    request.actorId(),
                    request.stableType(),
                    state,
                    request.restoreSnapshot(),
                    adapters,
                    cancellation,
                    new ZLinkBackendActorRef(
                        targetRid,
                        request.actorId(),
                        request.objectGeneration()))
                .thenApply(value -> value);
        }

        @Override public CompletionStage<Optional<byte[]>> replay(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            ZLinkActorAcceptedJournal.Record record) {
            assertTrue(open.get(),
                "D1 regular dispatch must stay closed until target open");
            EVENTS.add("replay:" + record.header().packetName());
            return CompletableFuture.completedFuture(Optional.empty());
        }

        @Override public CompletionStage<Void> stageTimers(
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            byte[] timerEnvelope) {
            EVENTS.add("target.timers.start");
            spots.stageEntryActorTimerRelocationEnvelope(
                request.targetSpotId(), request.actorId(), timerEnvelope);
            EVENTS.add("target.timers.done");
            return CompletableFuture.completedFuture(null);
        }

        @Override public void publish(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            fail("direct Join requires the committed owner generation");
        }

        @Override public void publish(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            long targetOwnerGeneration) {
            EVENTS.add("target.publish");
            actors.publishRelocatedActor(
                (ZLinkActorRuntime.PreparedTransferredActor) actor,
                request.targetSpotId(),
                targetOwnerGeneration);
        }

        @Override public void openAdmission(Object actor) {
            EVENTS.add("target.open");
            actors.openRelocatedActorAdmission(
                (ZLinkActorRuntime.PreparedTransferredActor) actor);
            open.set(true);
        }

        @Override public void publishTimers(
            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            spots.publishEntryActorTimerRelocation(
                request.targetSpotId(), request.actorId());
        }

        @Override public CompletionStage<Void> discard(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            var prepared = (ZLinkActorRuntime.PreparedTransferredActor) actor;
            return actors.discardRelocatedActor(prepared)
                .thenRun(() -> spots.discardEntryActorTimerRelocation(
                    request.targetSpotId(), prepared.actorId()));
        }
    }

    public static final class TrackingEntrySpot
        implements ZLinkEntrySpot<ZLinkActor> {
        private final ZLinkEntrySpotContext context;
        public TrackingEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }
        @Override public ZLinkEntrySpotContext context() { return context; }
        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            EVENTS.add("source.leave");
            return LEAVE_GATE.get();
        }
    }

    public static final class TrackingSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;
        public TrackingSpot(ZLinkSpotContext context) { this.context = context; }
        @Override public ZLinkSpotContext context() { return context; }
        @Override public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.accept());
        }
        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            EVENTS.add("target.joined");
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TrackingActor implements ZLinkActor {
        private final ZLinkActorContext context;
        public TrackingActor(ZLinkActorContext context) { this.context = context; }
        @Override public ZLinkActorContext context() { return context; }
        @Override public CompletionStage<Void> onJoinCompleted(
            ZLinkActorJoinCompletion completion) {
            var accepted = assertInstanceOf(
                ZLinkActorJoinCompletion.Accepted.class, completion);
            ACCEPTED.set(accepted.operationId());
            ACCEPTED_STAGE.get().complete(accepted.operationId());
            EVENTS.add("target.accepted");
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TrackingActorFactory
        implements ZLinkActorFactory {
        @Override public CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
                new TrackingActor(context));
        }
    }

    public static final class SnapshotAdapter
        implements ZLinkActorRelocationAdapter<TrackingActor> {
        @Override public CompletionStage<byte[]> capture(
            TrackingActor actor,
            ZLinkRelocationCancellation cancellation) {
            EVENTS.add("source.capture");
            return CompletableFuture.completedFuture(new byte[] {7, 2, 6});
        }
        @Override public CompletionStage<Void> restore(
            TrackingActor actor,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            assertArrayEquals(new byte[] {7, 2, 6}, state);
            EVENTS.add("target.restore");
            return CompletableFuture.completedFuture(null);
        }
    }
}
