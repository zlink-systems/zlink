package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

/**
 * Drives the canonical Actor Join relocation temporary queue (spec 15
 * §4.2) end to end through production entry points only:
 * {@link ZLinkActorJoinCanonicalAdapter#admit}, {@link
 * ZLinkUserSpotRetireTargetEndpoint#handleActor} and {@code stage}/{@code
 * publish}/{@code abort}. No test pokes {@code TemporaryQueue.offer(...)}
 * or any other private queue — every arrival is delivered the way real
 * ingress would deliver it.
 */
final class ZLinkActorJoinPrewarmIngressTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final String MESH = "prewarm-ingress";
    private static final String ACTOR_TYPE = "player";
    private static final String SPOT_TYPE = "room";
    private static final String TARGET_SPOT_ID = "room-a";
    private static final RoutingId SOURCE_RID = RoutingId.from("source-node");
    private static final RoutingId TARGET_RID = RoutingId.from("target-node");
    private static final long TARGET_NODE_GENERATION = 17;
    private static final long SOURCE_NODE_GENERATION = 11;

    @Test
    void arrivalBetweenAcceptedAndPrepareIsDeliveredAfterRelocationCompletes()
        throws Exception {
        try (Harness harness = Harness.start(
                request -> CompletableFuture.completedFuture(null))) {
            harness.admit();

            //  Step 2: a message for this exact Actor arrives at the
            //  target's production ingress endpoint before PREPARE
            //  (Restore) has installed the real staged queue. Without the
            //  fix this is dropped — handleActor only consulted the real
            //  stage map. With the fix it must be accepted (parked).
            var parkFailure = new AtomicReference<Throwable>();
            boolean parkedAccepted = harness.arrive(
                harness.sourceFence(), "EARLY", parkFailure);
            assertTrue(parkedAccepted,
                "production ingress must park an arrival between Accepted "
                    + "and PREPARE instead of dropping it");
            assertEquals(null, parkFailure.get());

            //  Step 3 (spec 15 §4.2 step 4): PREPARE (Restore) installs
            //  the real stage and must atomically migrate the parked
            //  arrival into it.
            harness.stageAndCommit();

            //  A further arrival after install must also reach the actor
            //  (through the real stage directly this time).
            boolean postInstallAccepted = harness.arrive(
                harness.sourceFence(), "AFTER_INSTALL", parkFailure);
            assertTrue(postInstallAccepted);
            assertEquals(null, parkFailure.get());

            //  Step 4 (spec 15 §4.2 step 7): publish commits and drains
            //  the durable backlog — the parked arrival, migrated at
            //  install time, must come out in order ahead of the arrival
            //  that landed straight in the real stage.
            harness.endpoint.publish(harness.request)
                .toCompletableFuture().get(5, TimeUnit.SECONDS);

            assertTrue(harness.joinedCalled.get(),
                "OnJoinedActor must run as part of the direct-Join publish");
            //  Both arrivals reached the actor, in order: the one parked
            //  between Accepted and PREPARE (migrated atomically when the
            //  real stage installed) ahead of the one that landed
            //  straight in the real stage after install.
            assertEquals(List.of("EARLY", "AFTER_INSTALL"),
                harness.recordingBackend.replayed);
        }
    }

    /**
     * Spec 04-session/02 §8.2: command 44 is submitted after the target
     * closed its relocation temporary queue and replayed it, and the
     * Session owner resumes relaying to the target route as soon as it
     * applies that command. The first relay therefore lands while the
     * target is still finishing publish (Location Store normalization,
     * stage release). The relocation temporary queue is closed by then, so
     * that arrival must fall through to normal Actor ingress — the endpoint
     * must not report it as staged and let the closed queue swallow it.
     */
    @Test
    void arrivalAfterTemporaryQueueClosedFallsThroughToNormalIngress()
        throws Exception {
        var normalizeEntered = new CompletableFuture<Void>();
        var normalizeRelease = new CompletableFuture<Void>();
        try (Harness harness = Harness.start(request -> {
                normalizeEntered.complete(null);
                return normalizeRelease;
            })) {
            harness.admit();
            harness.stageAndCommit();

            //  Publish closes the temporary queue, replays it and submits
            //  command 44, then blocks in normalization: this is the window
            //  in which the Session owner's first relay reaches the target.
            CompletionStage<Void> publish = harness.endpoint.publish(
                harness.request);
            normalizeEntered.get(5, TimeUnit.SECONDS);

            //  The relay arrives from the Session owner, not from the
            //  relocation source, so only the admission-time placeholder can
            //  claim it.
            var failure = new AtomicReference<Throwable>();
            boolean staged = harness.arrive(
                new ZLinkInternalMeshNode.PeerAuthorityFence(
                    RoutingId.from("session-owner-node"), 5, "session-owner", 2),
                "AFTER_CLOSE", failure);

            normalizeRelease.complete(null);
            publish.toCompletableFuture().get(5, TimeUnit.SECONDS);

            assertFalse(staged,
                "an arrival after the temporary queue closed must fall "
                    + "through to normal Actor ingress, not be reported as "
                    + "staged");
            assertEquals(null, failure.get());
            assertEquals(List.of(), harness.recordingBackend.replayed,
                "the closed temporary queue must not replay the late arrival");
        }
    }

    /**
     * One relocation of one Actor driven through the production target
     * endpoint: admission (spec 15 §4.2 step 2), PREPARE + target-only CAS
     * (steps 4/6) and publish (step 7) each run for real; only handler
     * dispatch is recorded instead of routed.
     */
    private static final class Harness implements AutoCloseable {
        final String actorId = "actor-prewarm";
        final long objectGeneration = 4;
        final long sourceAuthorityOwnerGeneration = 7;
        final UUID relocationId = UUID.randomUUID();
        final String actorAuthorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        final java.util.concurrent.atomic.AtomicBoolean joinedCalled =
            new java.util.concurrent.atomic.AtomicBoolean();
        final ZLinkFrameworkRuntime targetHost;
        final AuthorityState authority = new AuthorityState();
        final ZLinkAggregateRelocationCoordinator coordinator;
        final RecordingActorBackend recordingBackend;
        final ZLinkActorJoinCanonicalAdapter targetJoin;
        final ZLinkUserSpotRetireTargetEndpoint endpoint;
        final Object targetSpot;
        final ZLinkSpotRetireControl.StageRequest request;

        private Harness(
            ZLinkUserSpotRetireTargetEndpoint.SteadyNormalizer normalizer)
            throws Exception {
            var options = hostOptions();
            targetHost = ZLinkFrameworkRuntimeTestAccess.start(options);
            ZLinkSpotRuntime targetSpots =
                (ZLinkSpotRuntime) targetHost.spotManager();
            ZLinkActorRuntime targetActors =
                (ZLinkActorRuntime) targetHost.actorManager();
            targetHost.spotManager()
                .getOrCreate(TARGET_SPOT_ID, SPOT_TYPE)
                .submit().toCompletableFuture().get();
            targetSpot = targetSpots.spotFor(TARGET_SPOT_ID);
            var targetAdapters = new ZLinkRelocationAdapterRegistry(
                options.registration(), ZLinkHandlerActivator.reflection());

            authority.seedActorReady(
                actorAuthorityKey, objectGeneration,
                sourceAuthorityOwnerGeneration, "source-owner", 12,
                SOURCE_RID, SOURCE_NODE_GENERATION, "lobby", 3);
            ZLinkLocationRepository authorityStore = authority.proxy();
            coordinator = new ZLinkAggregateRelocationCoordinator(
                authorityStore);

            //  Production creation and lifecycle calls
            //  (prepare/openAdmission/publish/discard) run for real
            //  against the real Actor runtime; only the handler dispatch
            //  a real packet name would need is faked, since this test's
            //  focus is the relocation temporary queue plumbing, not
            //  application handler routing.
            recordingBackend = new RecordingActorBackend(
                targetSpots.primaryNode(),
                targetSpots.actorSessions(),
                targetAdapters,
                targetSpots);
            var actorStaging = new ZLinkStandaloneActorRelocationStagingOwner(
                recordingBackend);
            targetJoin = new ZLinkActorJoinCanonicalAdapter(
                targetActors, targetSpots);
            endpoint = new ZLinkUserSpotRetireTargetEndpoint(
                TARGET_RID,
                TARGET_NODE_GENERATION,
                coordinator,
                unusedSpotStaging(),
                ignored -> null,
                (lane, queued) -> CompletableFuture.failedFuture(
                    new AssertionError(
                        "no journal-backed saved/relayed record in this "
                            + "test — only parked (temporary) arrivals")),
                new ZLinkSessionRelocationPeerClient(inertNode()),
                Duration.ofSeconds(1),
                normalizer,
                null,
                null,
                authorityStore,
                actorStaging,
                targetJoin);

            var participants = List.of(new ZLinkSpotRetireControl
                .ParticipantFence(
                    actorAuthorityKey, 1, actorId, ACTOR_TYPE, true,
                    objectGeneration, sourceAuthorityOwnerGeneration));
            byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
                relocationId, actorId, objectGeneration,
                sourceAuthorityOwnerGeneration, true,
                new byte[] {7, 2, 6}, List.of());
            request = new ZLinkSpotRetireControl.StageRequest(
                new ZLinkSpotRetireControl.Fence(relocationId, 1),
                SOURCE_RID, SOURCE_NODE_GENERATION,
                "source-owner", 12,
                TARGET_RID, TARGET_NODE_GENERATION,
                "target-owner", 23,
                MESH, TARGET_SPOT_ID, ACTOR_TYPE, false, true,
                root,
                participants);
        }

        static Harness start(
            ZLinkUserSpotRetireTargetEndpoint.SteadyNormalizer normalizer)
            throws Exception {
            return new Harness(normalizer);
        }

        /** Spec 15 §4.2 step 2: OnActorJoin admits — the exact production
         * call the target makes before Accepted returns to source, and what
         * registers the relocation temporary queue. */
        void admit() {
            var admission = new ZLinkActorJoinRelocationPort.Admission(
                relocationId,
                new ZLinkActorJoinOperationId(1, 1),
                actorId,
                ACTOR_TYPE,
                objectGeneration,
                TARGET_SPOT_ID,
                targetSpot,
                actor -> {
                    joinedCalled.set(true);
                    return CompletableFuture.completedFuture(null);
                },
                ZLinkMessage.empty(),
                Duration.ofSeconds(30));
            targetJoin.admit(admission);
        }

        /** Spec 15 §4.2 steps 4 and 6: reserve the aggregate progress
         * marker, PREPARE (install the real stage), then the target-only
         * Location Store CAS — the same order production follows. */
        void stageAndCommit() throws Exception {
            var storeRequest = new ZLinkAggregateRelocationCoordinator
                .Request(
                    relocationId,
                    1,
                    1,
                    List.of(new ZLinkAggregateRelocationCoordinator
                        .Participant(
                            actorAuthorityKey,
                            ZLinkPlacementObjectKind.ACTOR,
                            objectGeneration,
                            sourceAuthorityOwnerGeneration,
                            "seed-1",
                            ZLinkAuthorityGenerationTransition.NEW_OWNER,
                            authority.rows.get(actorAuthorityKey).payload(),
                            new byte[0])),
                    request.relocationPayload(),
                    new ZLinkMeshNodeDescriptorKey(MESH, TARGET_RID),
                    TARGET_NODE_GENERATION,
                    new ZLinkPlacementCapacityBundle(1, 0, Optional.empty()),
                    new ZLinkLocationOwnerToken("target-owner", 23),
                    "seed-1");
            var prepared = coordinator.prepare(storeRequest, OPEN)
                .toCompletableFuture().get(5, TimeUnit.SECONDS);
            endpoint.stage(request).toCompletableFuture()
                .get(5, TimeUnit.SECONDS);
            coordinator.commit(prepared, OPEN)
                .toCompletableFuture().get(5, TimeUnit.SECONDS);
        }

        ZLinkInternalMeshNode.PeerAuthorityFence sourceFence() {
            return new ZLinkInternalMeshNode.PeerAuthorityFence(
                SOURCE_RID, SOURCE_NODE_GENERATION, "source-owner", 12);
        }

        /** One production ingress arrival for the relocating Actor. */
        boolean arrive(
            ZLinkInternalMeshNode.PeerAuthorityFence from,
            String packetName,
            AtomicReference<Throwable> failure) {
            byte[] record = actorRecord(actorId, packetName);
            return endpoint.handleActor(
                from,
                new ZLinkServiceM6BWireCodec.ActorMessage(
                    false, 0, null, 0, 0, 1, null,
                    new ZLinkServiceM6BWireCodec.ActorRouteFence(
                        new ZLinkBackendActorRef(
                            TARGET_RID, actorId, objectGeneration),
                        TARGET_NODE_GENERATION,
                        sourceAuthorityOwnerGeneration + 1,
                        23)),
                () -> record,
                List.of(Message.from(packetName)),
                null,
                reply -> { throw new AssertionError(
                    "a one-way arrival must not reply: " + packetName); },
                failure::set);
        }

        @Override public void close() throws Exception {
            targetHost.close();
        }
    }

    private static byte[] actorRecord(String actorId, String packetName) {
        return ZLinkAcceptedJournalTestRecords.actor(
            actorId, 0, packetName, Map.of(), new byte[] {1});
    }

    private static DefaultZLinkFrameworkOptions hostOptions() {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(
            new systems.zlink.framework.runtime.locations
                .ZLinkInMemoryLocationStore());
        options.addRelocationStore(new InMemoryRelocationStore());
        var mesh = options.addRouteMesh(MESH)
            .setRoutingId(TARGET_RID)
            .listen("inproc://prewarm-ingress-target");
        mesh.channelName(MESH).server();
        mesh.objects().server()
            .addSpotFactory(
                SPOT_TYPE,
                RoomSpot.class,
                factory -> factory.disableRelocation())
            .addActorFactory(
                ACTOR_TYPE,
                PlayerActor.class,
                PlayerActorFactory.class,
                factory -> factory.preserveStateWith(NoopAdapter.class));
        options.validate();
        return options;
    }

    private static ZLinkInternalMeshNode inertNode() {
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                if (method.getDeclaringClass() == Object.class) {
                    return method.invoke(proxy, arguments);
                }
                throw new UnsupportedOperationException(method.getName());
            });
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
                    systems.zlink.framework.actors.ZLinkRelocationCancellation
                        cancellation) { return unused(); }
                @Override public CompletionStage<Object> prepareActor(
                    ZLinkUserSpotAggregateStagingOwner.ActorParticipant actor,
                    systems.zlink.framework.actors.ZLinkRelocationCancellation
                        cancellation) { return unused(); }
                @Override public void publishSpot(Object spot) {
                    throw new AssertionError("unused");
                }
                @Override public void publishActor(Object actor) {
                    throw new AssertionError("unused");
                }
                @Override public void completeActor(Object actor) {
                    throw new AssertionError("unused");
                }
                @Override public void publishTimers(Object spot) {
                    throw new AssertionError("unused");
                }
                @Override public CompletionStage<Void> discardActor(
                    Object actor) {
                    return unused();
                }
                @Override public void discardSpot(Object spot) {
                    throw new AssertionError("unused");
                }
            });
    }

    /** Minimal fake authority store — proxy pattern mirrors the one in
     * {@code ZLinkUserSpotRetireTargetEndpointTest}, trimmed to a single
     * standalone Actor participant and adding direct row seeding so a
     * plausible pre-existing source-owned row can be set up without
     * driving a full creation flow. */
    /**
     * Delegates Actor creation and lifecycle (prepare/openAdmission/
     * publish/discard) to the real production {@link
     * ZLinkActorSessionCoordinator} so the whole relocation temporary
     * queue path runs against a real staged Actor. Only {@code replay}
     * is faked — it just records the packet name instead of dispatching
     * to a real handler method, since this test is not exercising
     * application handler routing.
     */
    private static final class RecordingActorBackend
        implements ZLinkStandaloneActorRelocationStagingOwner.Backend {
        private final systems.zlink.framework.runtime.internal.backend
            .ZLinkInternalSpotNode targetNode;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkRelocationAdapterRegistry adapters;
        private final ZLinkSpotRuntime spots;
        final List<String> replayed = new CopyOnWriteArrayList<>();

        RecordingActorBackend(
            systems.zlink.framework.runtime.internal.backend
                .ZLinkInternalSpotNode targetNode,
            ZLinkActorSessionCoordinator actors,
            ZLinkRelocationAdapterRegistry adapters,
            ZLinkSpotRuntime spots) {
            this.targetNode = targetNode;
            this.actors = actors;
            this.adapters = adapters;
            this.spots = spots;
        }

        @Override public CompletionStage<Object> prepare(
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            byte[] state,
            systems.zlink.framework.actors.ZLinkRelocationCancellation
                cancellation) {
            return actors.prepareRelocatedActor(
                    request.actorId(),
                    request.stableType(),
                    state,
                    request.restoreSnapshot(),
                    adapters,
                    cancellation,
                    new ZLinkBackendActorRef(
                        targetNode.routingId(),
                        request.actorId(),
                        request.objectGeneration()))
                .thenApply(value -> value);
        }

        @Override public CompletionStage<Optional<byte[]>> replay(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            ZLinkActorAcceptedJournal.Record record) {
            replayed.add(record.header().packetName());
            return CompletableFuture.completedFuture(Optional.empty());
        }

        @Override public void openAdmission(Object actor) {
            actors.openRelocatedActorAdmission(
                (ZLinkActorRuntime.PreparedTransferredActor) actor);
        }

        @Override public void publish(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            long targetOwnerGeneration) {
            actors.publishRelocatedActor(
                (ZLinkActorRuntime.PreparedTransferredActor) actor,
                request.targetSpotId(),
                targetOwnerGeneration);
        }

        @Override public CompletionStage<Void> discard(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            return actors.discardRelocatedActor(
                (ZLinkActorRuntime.PreparedTransferredActor) actor);
        }
    }

    private static final class AuthorityState {
        private final Map<String, ZLinkAuthoritySnapshot> rows =
            new ConcurrentHashMap<>();
        private ZLinkAggregatePrepareRequest prepared;
        private boolean progress;
        private String progressStoreVersion;

        void seedActorReady(
            String authorityKey,
            long objectGeneration,
            long authorityOwnerGeneration,
            String ownerId,
            long ownerLeaseGeneration,
            RoutingId nodeRid,
            long nodeGeneration,
            String currentSpotId,
            long currentSpotGeneration) {
            byte[] payload = new ZLinkActorAuthorityPayloadCodec().encode(
                ZLinkActorAuthorityPayloadCodec.State.READY,
                ACTOR_TYPE,
                "actor",
                currentSpotId,
                currentSpotGeneration,
                1,
                ownerId,
                ownerLeaseGeneration,
                MESH,
                nodeRid,
                nodeGeneration);
            rows.put(authorityKey, new ZLinkAuthoritySnapshot(
                "seed-1",
                payload,
                objectGeneration,
                authorityOwnerGeneration,
                ownerId,
                ownerLeaseGeneration,
                new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.ACTIVE,
                    ZLinkPlacementObjectKind.ACTOR,
                    ACTOR_TYPE,
                    new ZLinkMeshNodeDescriptorKey(MESH, nodeRid),
                    nodeGeneration,
                    new ZLinkPlacementCapacityBundle(
                        1, 0, Optional.empty())),
                Instant.now()));
        }

        ZLinkLocationRepository proxy() {
            return (ZLinkLocationRepository) Proxy.newProxyInstance(
                ZLinkLocationRepository.class.getClassLoader(),
                new Class<?>[] {ZLinkLocationRepository.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "prepareAggregate" -> prepare(
                        (ZLinkAggregatePrepareRequest) arguments[0]);
                    case "commitAggregate" -> commit(
                        (ZLinkAggregateFence) arguments[0]);
                    case "abortAggregate" -> CompletableFuture.completedFuture(
                        ZLinkAggregateAbortResult.ABORTED);
                    case "read" -> CompletableFuture.completedFuture(
                        rows.getOrDefault(
                            (String) arguments[0], null) == null
                            ? new ZLinkAuthorityMissing(Instant.now())
                            : rows.get((String) arguments[0]));
                    case "readAggregateProgress" -> readAggregateProgress(
                        (ZLinkAggregateFence) arguments[0]);
                    case "removeAggregateProgress" ->
                        removeAggregateProgress(
                            (ZLinkAggregateFence) arguments[0],
                            (String) arguments[1]);
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        }

        private CompletionStage<ZLinkAggregatePrepareResult> prepare(
            ZLinkAggregatePrepareRequest request) {
            prepared = request;
            return CompletableFuture.completedFuture(
                new ZLinkAggregatePrepared(new ZLinkAggregateFence(
                    request.aggregateId(), request.aggregateGeneration())));
        }

        private CompletionStage<ZLinkAggregateCommitResult> commit(
            ZLinkAggregateFence fence) {
            if (prepared == null
                || !prepared.aggregateId().equals(fence.aggregateId())
                || prepared.aggregateGeneration()
                    != fence.aggregateGeneration()) {
                return CompletableFuture.completedFuture(
                    ZLinkAggregateCommitResult.STALE);
            }
            for (var participant : prepared.participants()) {
                ZLinkAuthoritySnapshot current = rows.get(
                    participant.authorityKey());
                rows.put(participant.authorityKey(), new ZLinkAuthoritySnapshot(
                    "version-2",
                    participant.authorityPayload(),
                    current == null
                        ? 1 : current.objectGeneration(),
                    current == null
                        ? 1 : current.authorityOwnerGeneration() + 1,
                    prepared.targetOwner().ownerId(),
                    prepared.targetOwner().leaseGeneration(),
                    new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        ZLinkPlacementObjectKind.ACTOR,
                        ACTOR_TYPE,
                        prepared.targetDescriptor(),
                        prepared.targetDescriptorLifecycleGeneration(),
                        prepared.capacityBundle()),
                    Instant.now()));
            }
            progress = true;
            progressStoreVersion = "aggregate-commit";
            return CompletableFuture.completedFuture(
                ZLinkAggregateCommitResult.COMMITTED);
        }

        private CompletionStage<Optional<ZLinkAggregateProgressSnapshot>>
            readAggregateProgress(ZLinkAggregateFence fence) {
            return CompletableFuture.completedFuture(
                !progress
                    ? Optional.empty()
                    : Optional.of(new ZLinkAggregateProgressSnapshot(
                        fence, progressStoreVersion, prepared)));
        }

        private CompletionStage<Boolean> removeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion) {
            if (!progress
                || !progressStoreVersion.equals(expectedStoreVersion)) {
                return CompletableFuture.completedFuture(false);
            }
            progress = false;
            progressStoreVersion = null;
            return CompletableFuture.completedFuture(true);
        }
    }

    public static final class RoomSpot
        implements systems.zlink.framework.spots.ZLinkSpot<ZLinkActor> {
        private final systems.zlink.framework.spots.ZLinkSpotContext context;
        public RoomSpot(systems.zlink.framework.spots.ZLinkSpotContext context) {
            this.context = context;
        }
        @Override public systems.zlink.framework.spots.ZLinkSpotContext
            context() { return context; }
        @Override public CompletionStage<
            systems.zlink.framework.spots.ZLinkSpotActorJoinResult>
            onActorJoin(String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(
                systems.zlink.framework.spots.ZLinkSpotActorJoinResult
                    .accept());
        }
        @Override public CompletionStage<Void> onJoinedActor(
            ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(
            ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerActor implements ZLinkActor {
        private final systems.zlink.framework.actors.ZLinkActorContext context;
        public PlayerActor(
            systems.zlink.framework.actors.ZLinkActorContext context) {
            this.context = context;
        }
        @Override public systems.zlink.framework.actors.ZLinkActorContext
            context() { return context; }
        @Override public CompletionStage<Void> onJoinCompleted(
            systems.zlink.framework.actors.ZLinkActorJoinCompletion
                completion) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerActorFactory
        implements systems.zlink.framework.actors.ZLinkActorFactory {
        @Override public CompletionStage<ZLinkActor> create(
            systems.zlink.framework.actors.ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new PlayerActor(context));
        }
    }

    public static final class NoopAdapter
        implements systems.zlink.framework.actors.ZLinkActorRelocationAdapter<
            PlayerActor> {
        @Override public CompletionStage<byte[]> capture(
            PlayerActor actor,
            systems.zlink.framework.actors.ZLinkRelocationCancellation
                cancellation) {
            return CompletableFuture.completedFuture(new byte[] {7, 2, 6});
        }
        @Override public CompletionStage<Void> restore(
            PlayerActor actor,
            byte[] state,
            systems.zlink.framework.actors.ZLinkRelocationCancellation
                cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
