package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkServiceRelocationEnvelopeCodec;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.IntConsumer;

final class ZLinkCanonicalRelocationStateMachineTest {
    private static final Duration PREPARE_REPLY_WAIT = Duration.ofSeconds(3);
    private static final java.time.Instant STAGE_DEADLINE =
            java.time.Instant.parse("2100-01-01T00:00:00Z");
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @Test
    void boundRelocationWithNoPriorPeerWaitsForNativeAdmissionAndFirstPushSucceeds()
            throws Exception {
        try (var nativeRoute =
                new systems.zlink.framework.runtime.binding.ZLinkNativeSessionRouteFixture()) {
            var ready = nativeRoute.routeReady();
            assertFalse(ready.isDone());
            var deadlines = new java.util.ArrayList<Runnable>();
            Fixture f = fixture(null, new CountingEndpoint(), false, ready, deadlines);
            var request = f.boundRequest(nativeRoute.sessionOwnerGeneration());
            var staged =
                    f.source
                            .stage(f.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                            .toCompletableFuture();
            f.stateSubmitted.join();
            assertEquals(
                    List.of(
                            Map.entry(
                                    RoutingId.from("session-owner"),
                                    nativeRoute.sessionOwnerGeneration())),
                    f.securedRoutes);
            assertFalse(staged.isDone());
            assertFalse(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_READY));
            nativeRoute.connect();
            staged.get(5, java.util.concurrent.TimeUnit.SECONDS);
            assertTrue(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_READY));
            f.source
                    .publish(f.targetRid, request.fence(), Duration.ofSeconds(2))
                    .toCompletableFuture()
                    .join();
            assertEquals(1, f.endpoint.published.get());
            nativeRoute.firstPush();
        }
    }

    @Test
    void boundRelocationWithholdsReadyUntilSessionOwnerRouteIsReady() throws Exception {
        var route = new CompletableFuture<Void>();
        var deadlines = new java.util.ArrayList<Runnable>();
        Fixture f = fixture(null, new CountingEndpoint(), false, route, deadlines);
        var staged =
                f.source
                        .stage(f.targetRid, f.boundRequest(), Duration.ofSeconds(2), STAGE_DEADLINE)
                        .toCompletableFuture();
        f.stateSubmitted.join();
        assertEquals(1, f.endpoint.staged.get());
        assertEquals(List.of(Map.entry(RoutingId.from("session-owner"), 13L)), f.securedRoutes);
        var carriedPrepare =
                (ZLinkCanonicalRelocationProtocol.Prepare)
                        attemptMember(targetAttempt(f.target, f.boundRequest().fence()), "prepare");
        assertEquals(
                STAGE_DEADLINE.toEpochMilli(),
                carriedPrepare.deadlineUnixMs(),
                "command 40 must carry the source deadline unchanged");
        assertFalse(staged.isDone(), "relay-ready must wait for the Session owner route");
        assertFalse(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_READY));
        assertEquals(0, f.prepareInvocations.get());
        route.complete(null);
        staged.join();
        assertEquals(1, f.prepareInvocations.get());
        assertTrue(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_READY));
        assertEquals(
                0, f.endpoint.published.get(), "route readiness alone must not commit or publish");
    }

    @Test
    void sourceDeadlineFailsRouteWaitAndDiscardsStaging() {
        var route = new CompletableFuture<Void>();
        var deadlines = new java.util.ArrayList<Runnable>();
        var endpoint = new CountingEndpoint();
        Fixture f = fixture(null, endpoint, false, route, deadlines);
        var staged =
                f.source
                        .stage(f.targetRid, f.boundRequest(), Duration.ofSeconds(2), STAGE_DEADLINE)
                        .toCompletableFuture();
        f.stateSubmitted.join();
        assertFalse(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_READY));
        assertFalse(staged.isDone());
        assertEquals(
                0,
                f.prepareInvocations.get(),
                "route waiting must not prepare aggregate authority");
        assertEquals(1, deadlines.size());
        deadlines.get(0).run();
        var failure = assertThrows(java.util.concurrent.CompletionException.class, staged::join);
        var timeout =
                assertInstanceOf(java.util.concurrent.TimeoutException.class, failure.getCause());
        var classified = assertInstanceOf(ZLinkFrameworkException.class, timeout.getCause());
        assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, classified.kind());
        assertEquals(
                ServiceWireConstants.FRAMEWORK_ERROR_WORKER_TIMED_OUT,
                ZLinkRequestFailureMapping.causeCode(classified));
        assertTrue(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_FAILED));
        assertEquals(1, endpoint.aborted.get());
        assertTrue(route.isCancelled(), "deadline must remove the outstanding route observation");
        assertEquals(0, endpoint.published.get());
        assertEquals(
                0,
                f.prepareInvocations.get(),
                "route deadline must leave aggregate authority unprepared");
    }

    @Test
    void targetAdmissionSealCancelsOutstandingSessionRouteObservation() {
        var route = new CompletableFuture<Void>();
        var deadlines = new java.util.ArrayList<Runnable>();
        Fixture f = fixture(null, new CountingEndpoint(), false, route, deadlines);
        var staged =
                f.source
                        .stage(f.targetRid, f.boundRequest(), Duration.ofSeconds(2), STAGE_DEADLINE)
                        .toCompletableFuture();
        f.stateSubmitted.join();
        assertFalse(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_READY));
        assertFalse(staged.isDone());
        f.target.sealAcceptedTargetRelocations().toCompletableFuture().join();
        assertTrue(route.isCancelled());
        assertThrows(java.util.concurrent.CompletionException.class, staged::join);
        assertTrue(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_FAILED));
        assertEquals(1, f.endpoint.aborted.get());
        assertFalse(f.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_READY));
    }

    @Test
    void targetDrainWaitsForAdmissionResourceCleanup() throws Exception {
        var cleanupEntered = new CompletableFuture<Void>();
        var cleanup = new CompletableFuture<Void>();
        CountingEndpoint endpoint =
                new CountingEndpoint() {
                    @Override
                    public CompletionStage<Void> endPrepare(UUID relocationId) {
                        cleanupEntered.complete(null);
                        return cleanup;
                    }
                };
        Fixture fixture = fixture(null, endpoint);
        var request = fixture.request(new byte[] {1});
        fixture.source
                .stage(fixture.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();
        var publishing =
                fixture.source.publish(fixture.targetRid, request.fence(), Duration.ofSeconds(2));
        cleanupEntered.get(2, java.util.concurrent.TimeUnit.SECONDS);
        var drain = fixture.target.awaitAcceptedTargetRelocations().toCompletableFuture();
        try {
            assertThrows(
                    java.util.concurrent.TimeoutException.class,
                    () -> drain.get(100, java.util.concurrent.TimeUnit.MILLISECONDS),
                    "the target owns its resources until terminal cleanup finishes");
        } finally {
            cleanup.complete(null);
        }
        drain.get(2, java.util.concurrent.TimeUnit.SECONDS);
        publishing.toCompletableFuture().get(2, java.util.concurrent.TimeUnit.SECONDS);
    }

    @Test
    void targetDrainWaitsForAcceptedPublication() {
        CompletableFuture<Void> publication = new CompletableFuture<>();
        CountingEndpoint endpoint =
                new CountingEndpoint() {
                    @Override
                    public CompletionStage<Void> publish(
                            ZLinkSpotRetireControl.StageRequest request) {
                        super.publish(request);
                        return publication;
                    }
                };
        Fixture fixture = fixture(null, endpoint);
        ZLinkSpotRetireControl.StageRequest request = fixture.request(new byte[] {1});

        fixture.source
                .stage(fixture.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();
        CompletionStage<Void> publishing =
                fixture.source.publish(fixture.targetRid, request.fence(), Duration.ofSeconds(2));
        CompletionStage<Void> drain = fixture.target.awaitAcceptedTargetRelocations();

        assertEquals(1, endpoint.published.get());
        assertFalse(drain.toCompletableFuture().isDone(), "accepted publication must hold drain");
        publication.complete(null);
        publishing.toCompletableFuture().join();
        drain.toCompletableFuture().join();
    }

    @Test
    void drainingTargetRepliesFailedWithoutAdmittingPrepare() throws Exception {
        assertTargetRefusesPrepare(false);
    }

    @Test
    void hostSealedServingTargetRepliesFailedWithoutAdmittingPrepare() throws Exception {
        assertTargetRefusesPrepare(true);
    }

    private void assertTargetRefusesPrepare(boolean hostSealed) throws Exception {
        var drains =
                new systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator(
                        List.of("mesh"));
        Fixture fixture =
                fixture(
                        null,
                        new CountingEndpoint(),
                        false,
                        null,
                        null,
                        () -> drains.isSealed("mesh"));
        var request = fixture.request(new byte[] {1});
        fixture.source
                .stage(fixture.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();
        Object attempt = targetAttempt(fixture.target, request.fence());
        var accepted = (ZLinkCanonicalRelocationProtocol.Prepare) attemptMember(attempt, "prepare");
        if (hostSealed) drains.sealAll();
        else fixture.targetState.set(MeshNodeState.DRAINING);
        var late =
                new ZLinkCanonicalRelocationProtocol.Prepare(
                        UUID.randomUUID(),
                        accepted.targetAttemptGeneration(),
                        accepted.coordinator(),
                        accepted.target(),
                        accepted.initiatorRole(),
                        accepted.object(),
                        accepted.sourceNodeRid(),
                        accepted.sourceNodeGeneration(),
                        accepted.manifest(),
                        accepted.applicationVersion(),
                        accepted.deadlineUnixMs());
        byte[] encoded = ZLinkCanonicalRelocationProtocol.encodePrepare(late);

        byte[] reply =
                fixture.target
                        .apply(
                                fixture.sourceRid,
                                2L,
                                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                                encoded)
                        .toCompletableFuture()
                        .get(
                                PREPARE_REPLY_WAIT.toMillis(),
                                java.util.concurrent.TimeUnit.MILLISECONDS);
        var failed = ZLinkCanonicalRelocationProtocol.decodeFailed(reply);
        assertEquals(late.id(), failed.id());
        assertEquals(
                ServiceWireConstants.FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED, failed.failureCode());
        assertEquals(1, fixture.endpoint.staged.get());

        fixture.target
                .apply(
                        fixture.sourceRid,
                        null,
                        ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                        encoded)
                .toCompletableFuture()
                .join();
        assertTrue(fixture.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_FAILED));

        Fixture rejectedSource = fixture();
        rejectedSource.targetState.set(MeshNodeState.DRAINING);
        RuntimeException sourceFailure =
                assertThrows(
                        RuntimeException.class,
                        () ->
                                rejectedSource
                                        .source
                                        .stage(
                                                rejectedSource.targetRid,
                                                rejectedSource.request(new byte[] {1}),
                                                Duration.ofSeconds(2),
                                                STAGE_DEADLINE)
                                        .toCompletableFuture()
                                        .join());
        ZLinkFrameworkException frameworkFailure =
                assertInstanceOf(ZLinkFrameworkException.class, sourceFailure.getCause());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, frameworkFailure.kind());
        assertEquals(0, rejectedSource.endpoint.staged.get());
    }

    private static final AtomicReference<IntConsumer> canonicalControlHook =
            new AtomicReference<>();

    @Test
    void prepareReadyAndOneWayCutoverAreTheOnlyAttemptTransitions() {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        ZLinkLocationOwnerToken sourceOwner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                locations
                                        .claimOwnerLease("source-owner", Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        ZLinkLocationOwnerToken targetOwner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                locations
                                        .claimOwnerLease("target-owner", Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        locations
                .updateMeshNode(
                        descriptor(sourceRid, 11, sourceOwner, "source-entry"),
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        locations
                .updateMeshNode(
                        descriptor(targetRid, 12, targetOwner, "target-entry"),
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        byte[] sourceAuthority = actorAuthority(actorId, sourceRid, sourceOwner).payload();
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                locations
                                        .reserve(
                                                new ZLinkObjectReservationRequest(
                                                        ZLinkPlacementObjectKind.ACTOR,
                                                        authorityKey,
                                                        "actor-type",
                                                        "creation-root",
                                                        new byte[32],
                                                        32,
                                                        new ZLinkMeshNodeDescriptorKey(
                                                                "mesh", sourceRid),
                                                        11,
                                                        sourceOwner,
                                                        sourceAuthority,
                                                        ZLinkPlacementCapacityBundle.actor(1)),
                                                OPEN)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                locations.commit(reservation, sourceAuthority, OPEN).toCompletableFuture().join());
        ZLinkAuthoritySnapshot sourceSnapshot =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        locations.read(authorityKey, OPEN).toCompletableFuture().join());
        byte[] targetAuthority =
                new ZLinkActorAuthorityPayloadCodec()
                        .encode(
                                ZLinkActorAuthorityPayloadCodec.State.READY,
                                "actor-type",
                                actorId,
                                "target-entry",
                                12,
                                1,
                                targetOwner.ownerId(),
                                targetOwner.leaseGeneration(),
                                "mesh",
                                targetRid,
                                12);
        AtomicInteger commitAttempts = new AtomicInteger();
        ZLinkLocationRepository retryingLocations =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("commitAggregate")
                                            && commitAttempts.getAndIncrement() == 0) {
                                        return CompletableFuture.failedFuture(
                                                new IllegalStateException(
                                                        "transient Location Store failure"));
                                    }
                                    try {
                                        return method.invoke(locations, args);
                                    } catch (InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                });
        var coordinator = new ZLinkAggregateRelocationCoordinator(retryingLocations);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId,
                        actorId,
                        sourceSnapshot.objectGeneration(),
                        sourceSnapshot.authorityOwnerGeneration(),
                        true,
                        new byte[] {1},
                        List.of());
        AtomicReference<ZLinkCanonicalRelocationStateMachine> source = new AtomicReference<>();
        AtomicReference<ZLinkCanonicalRelocationStateMachine> target = new AtomicReference<>();
        var sourceCommands = new CopyOnWriteArrayList<Integer>();
        var targetCommands = new CopyOnWriteArrayList<Integer>();
        CountingEndpoint endpoint = new CountingEndpoint();
        source.set(
                new ZLinkCanonicalRelocationStateMachine(
                        node(sourceRid, 11, target, sourceCommands),
                        "mesh",
                        "source-entry",
                        retryingLocations,
                        coordinator,
                        new CountingEndpoint()));
        target.set(
                new ZLinkCanonicalRelocationStateMachine(
                        node(targetRid, 12, source, targetCommands),
                        "mesh",
                        "target-entry",
                        retryingLocations,
                        coordinator,
                        endpoint));
        var request =
                new ZLinkSpotRetireControl.StageRequest(
                        new ZLinkSpotRetireControl.Fence(relocationId, 1),
                        sourceRid,
                        11,
                        sourceOwner.ownerId(),
                        sourceOwner.leaseGeneration(),
                        targetRid,
                        12,
                        targetOwner.ownerId(),
                        targetOwner.leaseGeneration(),
                        "mesh",
                        "target-entry",
                        "actor-type",
                        false,
                        true,
                        root,
                        List.of(
                                new ZLinkSpotRetireControl.ParticipantFence(
                                        authorityKey,
                                        1,
                                        actorId,
                                        "actor-type",
                                        true,
                                        sourceSnapshot.objectGeneration(),
                                        sourceSnapshot.authorityOwnerGeneration())),
                        List.of());

        source.get()
                .stage(targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();
        source.get()
                .relay(targetRid, request.fence(), new byte[] {1, 2, 3}, Duration.ofSeconds(2))
                .toCompletableFuture()
                .join();
        source.get()
                .publish(targetRid, request.fence(), Duration.ofSeconds(2))
                .toCompletableFuture()
                .join();

        assertEquals(
                List.of(
                        ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                        ServiceWireConstants.COMMAND_RELOCATION_STATE,
                        ServiceWireConstants.COMMAND_RELOCATION_DATA,
                        ServiceWireConstants.COMMAND_RELOCATION_CUTOVER),
                sourceCommands);
        assertEquals(List.of(ServiceWireConstants.COMMAND_RELOCATION_READY), targetCommands);
        assertEquals(1, endpoint.staged.get());
        assertEquals(1, endpoint.published.get());
        assertEquals(1, endpoint.relayed.get());
        assertEquals(
                2,
                commitAttempts.get(),
                "the target retries the same CAS fence after a retryable failure");
        ZLinkAuthoritySnapshot published =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        locations.read(authorityKey, OPEN).toCompletableFuture().join());
        assertEquals(
                targetOwner.ownerId(),
                published.ownerId(),
                "only the target command handler performs the owner CAS");
    }

    /**
     * Spec 15 §4.2: the target's advertised relocation state chunk receive limit additionally
     * bounds this relocation's chunk size — proven by observing more command 52 chunks for a small
     * advertised limit than for the unadvertised (node-budget-only) baseline over an identical
     * payload.
     */
    @Test
    void advertisedReceiveChunkLimitIsAppliedAsAMinimumOnTheSourceChunkPlan() {
        byte[] payload = new byte[20];
        java.util.Arrays.fill(payload, (byte) 7);

        Fixture unadvertised = fixture();
        var baselineRequest = unadvertised.requestWithAdvertisedLimit(payload, 0L);
        unadvertised
                .source
                .stage(
                        unadvertised.targetRid,
                        baselineRequest,
                        Duration.ofSeconds(2),
                        STAGE_DEADLINE)
                .toCompletableFuture()
                .join();
        long baselineChunks =
                unadvertised.sourceCommands.stream()
                        .filter(command -> command == ServiceWireConstants.COMMAND_RELOCATION_STATE)
                        .count();

        Fixture advertised = fixture();
        var boundedRequest = advertised.requestWithAdvertisedLimit(payload, 4L);
        advertised
                .source
                .stage(advertised.targetRid, boundedRequest, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();
        long boundedChunks =
                advertised.sourceCommands.stream()
                        .filter(command -> command == ServiceWireConstants.COMMAND_RELOCATION_STATE)
                        .count();

        int rootLength = unadvertised.root(payload).length;
        long expectedBoundedChunks = (rootLength + 3) / 4;
        assertEquals(
                1,
                baselineChunks,
                "the encoded relocation root fits in one chunk under the node" + " budget alone");
        assertEquals(
                expectedBoundedChunks,
                boundedChunks,
                "a 4-byte advertised limit splits the same root into ceil(len/4)" + " chunks");
        assertTrue(
                boundedChunks > baselineChunks,
                "the advertised limit measurably increases the chunk count");
    }

    @Test
    void wireFailureCodeMapsAChecksumFailureToRelocationDataLost() {
        assertEquals(
                ServiceWireConstants.FRAMEWORK_ERROR_RELOCATION_DATA_LOST,
                ZLinkCanonicalRelocationStateMachine.wireFailureCode(
                        new systems.zlink.framework.errors.ZLinkFrameworkException(
                                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.DATA_LOST,
                                "relocation base payload checksum differs from the "
                                        + "manifest")));
    }

    @Test
    void completedStagePublishReentrySubmitsTheRetireEndpointOnlyOnce() throws Exception {
        Fixture fixture = fixture();
        AtomicReference<ZLinkSpotRetireControl.Target> target = new AtomicReference<>();
        AtomicInteger publishes = new AtomicInteger();
        AtomicBoolean reentered = new AtomicBoolean();
        AtomicReference<CompletionStage<byte[]>> reentrantPublication = new AtomicReference<>();
        ZLinkSpotRetireControl.StageRequest request = fixture.request(new byte[] {1});
        byte[] publish = retireControlCodec("encodeFence", 2, request.fence());
        ZLinkSpotRetireControl.TargetEndpoint endpoint =
                new ZLinkSpotRetireControl.TargetEndpoint() {
                    @Override
                    public CompletionStage<Void> stage(
                            ZLinkSpotRetireControl.StageRequest request) {
                        return CompletableFuture.completedFuture(null);
                    }

                    @Override
                    public CompletionStage<Void> publish(
                            ZLinkSpotRetireControl.StageRequest request) {
                        publishes.incrementAndGet();
                        if (reentered.compareAndSet(false, true)) {
                            reentrantPublication.set(
                                    target.get().handle(request.sourceNodeRid(), publish));
                        }
                        return CompletableFuture.completedFuture(null);
                    }

                    @Override
                    public CompletionStage<Void> stageRelayedRecord(
                            ZLinkSpotRetireControl.StageRequest request, byte[] frozenRecord) {
                        return CompletableFuture.completedFuture(null);
                    }

                    @Override
                    public CompletionStage<Void> abort(
                            ZLinkSpotRetireControl.StageRequest request) {
                        return CompletableFuture.completedFuture(null);
                    }
                };
        target.set(newRetireTarget(endpoint));

        target.get()
                .handle(request.sourceNodeRid(), retireControlCodec("encodeStage", request))
                .toCompletableFuture()
                .join();
        CompletionStage<byte[]> firstPublication =
                target.get().handle(request.sourceNodeRid(), publish);
        firstPublication.toCompletableFuture().join();
        reentrantPublication.get().toCompletableFuture().join();

        assertEquals(
                1,
                publishes.get(),
                "the completed stage's inline publish callback must observe the"
                        + " already-claimed retire publication");
        assertSame(
                firstPublication,
                reentrantPublication.get(),
                "the reentry must share the exact retire publication stage");
    }

    @Test
    void completedReadyPrefixReentryArmsOneReadyPublicationForTheFence() throws Exception {
        Fixture fixture = fixture();
        var request = fixture.request(new byte[] {1});
        AtomicBoolean reentered = new AtomicBoolean();
        AtomicReference<Object> reentrantPublication = new AtomicReference<>();
        CountDownLatch reenteredReady = new CountDownLatch(1);
        canonicalControlHook.set(
                command -> {
                    if (command == ServiceWireConstants.COMMAND_RELOCATION_READY
                            && reentered.compareAndSet(false, true)) {
                        try {
                            reentrantPublication.set(
                                    invokeCanonicalPublication(
                                            fixture.target,
                                            "publishReady",
                                            targetFence(fixture.target),
                                            targetAttempt(fixture.target, request.fence()),
                                            request.sourceNodeRid()));
                            reenteredReady.countDown();
                        } catch (Exception failure) {
                            throw new AssertionError(failure);
                        }
                    }
                });
        try {
            fixture.source
                    .stage(fixture.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                    .toCompletableFuture()
                    .join();
            assertTrue(reenteredReady.await(2, java.util.concurrent.TimeUnit.SECONDS));
        } finally {
            canonicalControlHook.set(null);
        }

        Object attempt = targetAttempt(fixture.target, request.fence());
        assertSame(
                attemptMember(attempt, "readyPublication"),
                reentrantPublication.get(),
                "the completed ready prefix must share the exact READY publication");
    }

    @Test
    void completedTargetPublishPrefixReentrySubmitsTheFenceOnlyOnce() throws Exception {
        Fixture fixture = fixture();
        var request = fixture.request(new byte[] {1, 2, 3});
        AtomicBoolean reentered = new AtomicBoolean();
        fixture.endpoint.onPublish =
                () -> {
                    if (reentered.compareAndSet(false, true)) {
                        try {
                            fixture.target
                                    .apply(
                                            request.sourceNodeRid(),
                                            ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
                                            (byte[])
                                                    attemptMember(
                                                            targetAttempt(
                                                                    fixture.target,
                                                                    request.fence()),
                                                            "receivedCutover"))
                                    .toCompletableFuture()
                                    .join();
                        } catch (Exception failure) {
                            throw new AssertionError(failure);
                        }
                    }
                };

        fixture.source
                .stage(fixture.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();
        fixture.source
                .relay(
                        fixture.targetRid,
                        request.fence(),
                        new byte[] {1, 2, 3},
                        Duration.ofSeconds(2))
                .toCompletableFuture()
                .join();
        fixture.source
                .publish(fixture.targetRid, request.fence(), Duration.ofSeconds(2))
                .toCompletableFuture()
                .join();

        assertEquals(
                1,
                fixture.endpoint.published.get(),
                "the completed target prefix must not submit the same fence twice");
    }

    @Test
    void wireFailureCodeMapsInternalFailureToTheClosestDefinedWireCode() {
        assertEquals(
                ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_FAILED,
                ZLinkCanonicalRelocationStateMachine.wireFailureCode(
                        new systems.zlink.framework.errors.ZLinkFrameworkException(
                                systems.zlink.framework.errors.ZLinkFrameworkErrorKind
                                        .INTERNAL_FAILURE,
                                "Capture/factory/restore/staging failed internally")));
    }

    @Test
    void wireFailureCodeTreatsAnUnclassifiedThrowableAsOpaqueRequestFailure() {
        assertEquals(
                ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_FAILED,
                ZLinkCanonicalRelocationStateMachine.wireFailureCode(
                        new IllegalStateException("opaque failure")));
    }

    @Test
    void wireFailureKindDecodesWorkerTimedOutToDeadlineExceeded() {
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                ZLinkCanonicalRelocationStateMachine.wireFailureKind(
                        ServiceWireConstants.FRAMEWORK_ERROR_WORKER_TIMED_OUT));
    }

    @Test
    void wireFailureKindDecodesWorkerQueueFullToUnavailable() {
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                ZLinkCanonicalRelocationStateMachine.wireFailureKind(
                        ServiceWireConstants.FRAMEWORK_ERROR_WORKER_QUEUE_FULL));
    }

    @Test
    void wireFailureKindDecodesRelocationDataLostToDataLost() {
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.DATA_LOST,
                ZLinkCanonicalRelocationStateMachine.wireFailureKind(
                        ServiceWireConstants.FRAMEWORK_ERROR_RELOCATION_DATA_LOST));
    }

    @Test
    void wireFailureKindDecodesRequestRejectedToRejected() {
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                ZLinkCanonicalRelocationStateMachine.wireFailureKind(
                        ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_REJECTED));
    }

    @Test
    void wireFailureKindFallsBackToInternalFailureForAnUnknownCode() {
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                ZLinkCanonicalRelocationStateMachine.wireFailureKind(999L));
    }

    @Test
    void unregisteredAdapterTakesTheUnchangedFullPayloadPath() {
        Fixture fixture = fixture();
        var request = fixture.request(new byte[] {1});

        fixture.source
                .stage(fixture.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();

        assertArrayEquals(
                fixture.root(new byte[] {1}),
                fixture.endpoint.lastStaged.get().relocationPayload());
    }

    @Test
    void preStageCleanupExceptionDoesNotSuppressTheFailedReply() {
        FailingCleanupEndpoint endpoint = new FailingCleanupEndpoint();
        Fixture fixture = fixture(null, endpoint);

        assertThrows(
                java.util.concurrent.CompletionException.class,
                () ->
                        fixture.source
                                .stage(
                                        fixture.targetRid,
                                        fixture.request(new byte[] {1}),
                                        Duration.ofSeconds(2),
                                        STAGE_DEADLINE)
                                .toCompletableFuture()
                                .join());

        assertTrue(
                fixture.targetCommands.contains(ServiceWireConstants.COMMAND_RELOCATION_FAILED),
                "the target must send FAILED even when pre-stage cleanup throws");
    }

    @Test
    void actorJoinPrepareRetriesNotConnectedWithinOriginalDeadline() throws Exception {
        Fixture fixture = fixtureWithTransientPrepareDisconnect();

        fixture.source
                .stage(
                        fixture.targetRid,
                        fixture.request(new byte[] {1}),
                        Duration.ofSeconds(2),
                        STAGE_DEADLINE)
                .toCompletableFuture()
                .get(3, java.util.concurrent.TimeUnit.SECONDS);

        assertEquals(
                2L,
                fixture.sourceCommands.stream()
                        .filter(
                                command ->
                                        command == ServiceWireConstants.COMMAND_RELOCATION_PREPARE)
                        .count());
        assertEquals(
                1,
                fixture.endpoint.staged.get(),
                "the retried exact PREPARE must retain one target stage");
    }

    @Test
    void conflictingPrepareRepliesFailedWithoutReplacingTheOriginalStage() {
        Fixture fixture = fixture();
        var request = fixture.request(new byte[] {1});
        var prepare = prepare(fixture, request);
        fixture.source
                .stage(fixture.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();

        assertPrepareConflictReply(fixture, prepare);
        byte[] exactReply =
                fixture.target
                        .apply(
                                fixture.sourceRid,
                                3L,
                                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                                ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))
                        .toCompletableFuture()
                        .join();
        assertEquals(prepare.id(), ZLinkCanonicalRelocationProtocol.decodeReady(exactReply).id());
        assertEquals(1, fixture.endpoint.staged.get());
        fixture.source
                .publish(fixture.targetRid, request.fence(), Duration.ofSeconds(2))
                .toCompletableFuture()
                .join();
        assertEquals(1, fixture.endpoint.published.get());
    }

    @Test
    void exactPrepareRequestAfterTargetCutoverRepliesReady() throws Exception {
        Fixture fixture = fixture();
        var request = fixture.request(new byte[] {1});
        var prepare = prepare(fixture, request);

        fixture.source
                .stage(fixture.targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                .toCompletableFuture()
                .join();
        byte[] encodedPrepare = ZLinkCanonicalRelocationProtocol.encodePrepare(prepare);
        fixture.source
                .publish(fixture.targetRid, request.fence(), Duration.ofSeconds(2))
                .toCompletableFuture()
                .join();

        byte[] encodedReady =
                fixture.target
                        .apply(
                                fixture.sourceRid,
                                2L,
                                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                                encodedPrepare)
                        .toCompletableFuture()
                        .join();
        var ready = ZLinkCanonicalRelocationProtocol.decodeReady(encodedReady);

        assertEquals(prepare.id(), ready.id());
        assertEquals(ZLinkCanonicalRelocationProtocol.TARGET, ready.senderRole());
        assertEquals(
                1,
                fixture.endpoint.staged.get(),
                "an exact terminal PREPARE must not stage the target again");
        assertPrepareConflictReply(fixture, prepare);
        assertEquals(1, fixture.endpoint.staged.get());
    }

    private static void assertPrepareConflictReply(
            Fixture fixture, ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        var conflicting =
                new ZLinkCanonicalRelocationProtocol.Prepare(
                        prepare.id(),
                        prepare.targetAttemptGeneration(),
                        prepare.coordinator(),
                        prepare.target(),
                        prepare.initiatorRole(),
                        prepare.object(),
                        prepare.sourceNodeRid(),
                        prepare.sourceNodeGeneration(),
                        new ZLinkCanonicalRelocationProtocol.Manifest(
                                prepare.manifest().totalLength(),
                                prepare.manifest().chunkCount(),
                                prepare.manifest().checksumCrc32c() ^ 1),
                        prepare.applicationVersion(),
                        prepare.deadlineUnixMs());
        byte[] reply =
                fixture.target
                        .apply(
                                fixture.sourceRid,
                                2L,
                                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                                ZLinkCanonicalRelocationProtocol.encodePrepare(conflicting))
                        .toCompletableFuture()
                        .join();
        var failed = ZLinkCanonicalRelocationProtocol.decodeFailed(reply);
        assertEquals(prepare.id(), failed.id());
        assertEquals(prepare.targetAttemptGeneration(), failed.targetAttemptGeneration());
        assertEquals(prepare.coordinator(), failed.coordinator());
        assertEquals(prepare.target(), failed.target());
        assertEquals(prepare.object(), failed.object());
        assertEquals(
                ServiceWireConstants.FRAMEWORK_ERROR_RELOCATION_DATA_LOST, failed.failureCode());
        var differentIdentity =
                new ZLinkCanonicalRelocationProtocol.Prepare(
                        conflicting.id(),
                        conflicting.targetAttemptGeneration(),
                        new ZLinkCanonicalRelocationProtocol.Coordinator(
                                prepare.coordinator().ownerId(),
                                prepare.coordinator().ownerLeaseGeneration(),
                                prepare.coordinator().nodeRid(),
                                prepare.coordinator().nodeGeneration(),
                                prepare.coordinator().expectedAuthorityStoreVersion()
                                        + "-different"),
                        conflicting.target(),
                        conflicting.initiatorRole(),
                        conflicting.object(),
                        conflicting.sourceNodeRid(),
                        conflicting.sourceNodeGeneration(),
                        conflicting.manifest(),
                        conflicting.applicationVersion(),
                        conflicting.deadlineUnixMs());
        assertNull(
                fixture.target
                        .apply(
                                fixture.sourceRid,
                                4L,
                                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                                ZLinkCanonicalRelocationProtocol.encodePrepare(differentIdentity))
                        .toCompletableFuture()
                        .join());
        var differentTarget =
                new ZLinkCanonicalRelocationProtocol.Prepare(
                        conflicting.id(),
                        conflicting.targetAttemptGeneration(),
                        conflicting.coordinator(),
                        new ZLinkCanonicalRelocationProtocol.Target(
                                prepare.target().nodeRid(),
                                prepare.target().nodeGeneration(),
                                prepare.target().ownerId() + "-different",
                                prepare.target().ownerLeaseGeneration()),
                        conflicting.initiatorRole(),
                        conflicting.object(),
                        conflicting.sourceNodeRid(),
                        conflicting.sourceNodeGeneration(),
                        conflicting.manifest(),
                        conflicting.applicationVersion(),
                        conflicting.deadlineUnixMs());
        assertNull(
                fixture.target
                        .apply(
                                fixture.sourceRid,
                                5L,
                                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                                ZLinkCanonicalRelocationProtocol.encodePrepare(differentTarget))
                        .toCompletableFuture()
                        .join());
    }

    private static ZLinkCanonicalRelocationProtocol.Prepare prepare(
            Fixture fixture, ZLinkSpotRetireControl.StageRequest request) {
        var participant = request.participants().getFirst();
        byte[] payload = request.relocationPayload();
        return new ZLinkCanonicalRelocationProtocol.Prepare(
                request.fence().aggregateId(),
                request.fence().aggregateGeneration(),
                new ZLinkCanonicalRelocationProtocol.Coordinator(
                        request.sourceOwnerId(),
                        request.sourceOwnerLeaseGeneration(),
                        request.sourceNodeRid(),
                        request.sourceNodeGeneration(),
                        fixture.sourceSnapshot.storeVersion()),
                new ZLinkCanonicalRelocationProtocol.Target(
                        request.targetNodeRid(),
                        request.targetNodeGeneration(),
                        request.targetOwnerId(),
                        request.targetOwnerLeaseGeneration()),
                ZLinkCanonicalRelocationProtocol.SOURCE,
                new ZLinkCanonicalRelocationProtocol.ObjectFence(
                        participant.objectKind(),
                        participant.objectId(),
                        "",
                        participant.objectGeneration(),
                        participant.sourceAuthorityOwnerGeneration()),
                request.sourceNodeRid(),
                request.sourceNodeGeneration(),
                ZLinkRelocationPayloadTransfer.manifest(payload, payload.length),
                ZLinkServiceRelocationEnvelopeCodec.decode(payload).applicationVersion(),
                STAGE_DEADLINE.toEpochMilli());
    }

    private Fixture fixture() {
        return fixture(null);
    }

    private Fixture fixtureWithTransientPrepareDisconnect() {
        return fixture(null, new CountingEndpoint(), true);
    }

    /**
     * Builds one wired source/target pair with a committed source Actor authority ready for PREPARE
     * — the shared setup underlying the tests above.
     */
    private Fixture fixture(
            ZLinkCanonicalRelocationStateMachine.RetentionScheduler retentionScheduler) {
        return fixture(retentionScheduler, new CountingEndpoint());
    }

    private Fixture fixture(
            ZLinkCanonicalRelocationStateMachine.RetentionScheduler retentionScheduler,
            CountingEndpoint endpoint) {
        return fixture(retentionScheduler, endpoint, false);
    }

    private Fixture fixture(
            ZLinkCanonicalRelocationStateMachine.RetentionScheduler retentionScheduler,
            CountingEndpoint endpoint,
            boolean disconnectFirstPrepare) {
        return fixture(retentionScheduler, endpoint, disconnectFirstPrepare, null, null);
    }

    private Fixture fixture(
            ZLinkCanonicalRelocationStateMachine.RetentionScheduler retentionScheduler,
            CountingEndpoint endpoint,
            boolean disconnectFirstPrepare,
            CompletableFuture<Void> sessionRoute,
            List<Runnable> routeDeadlines) {
        return fixture(
                retentionScheduler,
                endpoint,
                disconnectFirstPrepare,
                sessionRoute,
                routeDeadlines,
                () -> false);
    }

    private Fixture fixture(
            ZLinkCanonicalRelocationStateMachine.RetentionScheduler retentionScheduler,
            CountingEndpoint endpoint,
            boolean disconnectFirstPrepare,
            CompletableFuture<Void> sessionRoute,
            List<Runnable> routeDeadlines,
            java.util.function.BooleanSupplier targetAdmissionSealed) {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        ZLinkLocationOwnerToken sourceOwner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                locations
                                        .claimOwnerLease("source-owner", Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        ZLinkLocationOwnerToken targetOwner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                locations
                                        .claimOwnerLease("target-owner", Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        locations
                .updateMeshNode(
                        descriptor(sourceRid, 11, sourceOwner, "source-entry"),
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        locations
                .updateMeshNode(
                        descriptor(targetRid, 12, targetOwner, "target-entry"),
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        byte[] sourceAuthority = actorAuthority(actorId, sourceRid, sourceOwner).payload();
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                locations
                                        .reserve(
                                                new ZLinkObjectReservationRequest(
                                                        ZLinkPlacementObjectKind.ACTOR,
                                                        authorityKey,
                                                        "actor-type",
                                                        "creation-root",
                                                        new byte[32],
                                                        32,
                                                        new ZLinkMeshNodeDescriptorKey(
                                                                "mesh", sourceRid),
                                                        11,
                                                        sourceOwner,
                                                        sourceAuthority,
                                                        ZLinkPlacementCapacityBundle.actor(1)),
                                                OPEN)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                locations.commit(reservation, sourceAuthority, OPEN).toCompletableFuture().join());
        ZLinkAuthoritySnapshot sourceSnapshot =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        locations.read(authorityKey, OPEN).toCompletableFuture().join());
        var prepareInvocations = new AtomicInteger();
        ZLinkLocationRepository tracedLocations =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("prepareAggregate"))
                                        prepareInvocations.incrementAndGet();
                                    try {
                                        return method.invoke(locations, args);
                                    } catch (InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                });
        var coordinator = new ZLinkAggregateRelocationCoordinator(tracedLocations);
        UUID relocationId = UUID.randomUUID();
        AtomicReference<ZLinkCanonicalRelocationStateMachine> source = new AtomicReference<>();
        AtomicReference<ZLinkCanonicalRelocationStateMachine> target = new AtomicReference<>();
        var sourceCommands = new CopyOnWriteArrayList<Integer>();
        var targetCommands = new CopyOnWriteArrayList<Integer>();
        var targetState = new AtomicReference<>(MeshNodeState.READY);
        var stateSubmitted = new CompletableFuture<Void>();
        var securedRoutes = new CopyOnWriteArrayList<Map.Entry<RoutingId, Long>>();
        ZLinkInternalMeshNode sourceNode =
                disconnectFirstPrepare
                        ? requestReplyNode(sourceRid, 11, target, sourceCommands)
                        : node(
                                sourceRid,
                                11,
                                target,
                                sourceCommands,
                                new AtomicReference<>(MeshNodeState.READY),
                                CompletableFuture.completedFuture(null),
                                new CopyOnWriteArrayList<>(),
                                stateSubmitted);
        if (disconnectFirstPrepare) {
            ZLinkCanonicalRelocationStateMachine.RetentionScheduler retention =
                    (deadline, cleanup) -> {};
            ZLinkCanonicalRelocationStateMachine.DelayScheduler delay = (duration, work) -> {};
            source.set(
                    new ZLinkCanonicalRelocationStateMachine(
                            sourceNode,
                            "mesh",
                            "source-entry",
                            locations,
                            coordinator,
                            new CountingEndpoint(),
                            retention,
                            ZLinkRelocationPayloadTransfer.Options.defaults(),
                            () -> 1L,
                            delay));
            target.set(
                    new ZLinkCanonicalRelocationStateMachine(
                            node(targetRid, 12, source, targetCommands, targetState),
                            "mesh",
                            "target-entry",
                            locations,
                            coordinator,
                            endpoint,
                            retention,
                            ZLinkRelocationPayloadTransfer.Options.defaults(),
                            () -> 1L,
                            delay));
        } else if (retentionScheduler == null) {
            source.set(
                    new ZLinkCanonicalRelocationStateMachine(
                            sourceNode,
                            "mesh",
                            "source-entry",
                            locations,
                            coordinator,
                            new CountingEndpoint()));
            target.set(
                    new ZLinkCanonicalRelocationStateMachine(
                            node(targetRid, 12, source, targetCommands, targetState),
                            "mesh",
                            "target-entry",
                            locations,
                            coordinator,
                            endpoint,
                            targetAdmissionSealed));
        } else {
            source.set(
                    new ZLinkCanonicalRelocationStateMachine(
                            sourceNode,
                            "mesh",
                            "source-entry",
                            locations,
                            coordinator,
                            new CountingEndpoint(),
                            retentionScheduler));
            target.set(
                    new ZLinkCanonicalRelocationStateMachine(
                            node(targetRid, 12, source, targetCommands, targetState),
                            "mesh",
                            "target-entry",
                            locations,
                            coordinator,
                            endpoint,
                            retentionScheduler));
        }
        if (sessionRoute != null) {
            target.set(
                    new ZLinkCanonicalRelocationStateMachine(
                            node(
                                    targetRid,
                                    12,
                                    source,
                                    targetCommands,
                                    targetState,
                                    sessionRoute,
                                    securedRoutes,
                                    new CompletableFuture<>()),
                            "mesh",
                            "target-entry",
                            locations,
                            coordinator,
                            endpoint,
                            (deadline, cleanup) -> {},
                            ZLinkRelocationPayloadTransfer.Options.defaults(),
                            System::nanoTime,
                            (delay, work) -> routeDeadlines.add(work)));
        }
        return new Fixture(
                sourceRid,
                targetRid,
                actorId,
                authorityKey,
                sourceOwner,
                targetOwner,
                sourceSnapshot,
                relocationId,
                source.get(),
                target.get(),
                endpoint,
                sourceCommands,
                targetCommands,
                targetState,
                stateSubmitted,
                securedRoutes,
                prepareInvocations);
    }

    private record Fixture(
            RoutingId sourceRid,
            RoutingId targetRid,
            String actorId,
            String authorityKey,
            ZLinkLocationOwnerToken sourceOwner,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkAuthoritySnapshot sourceSnapshot,
            UUID relocationId,
            ZLinkCanonicalRelocationStateMachine source,
            ZLinkCanonicalRelocationStateMachine target,
            CountingEndpoint endpoint,
            List<Integer> sourceCommands,
            List<Integer> targetCommands,
            AtomicReference<MeshNodeState> targetState,
            CompletableFuture<Void> stateSubmitted,
            List<Map.Entry<RoutingId, Long>> securedRoutes,
            AtomicInteger prepareInvocations) {
        ZLinkSpotRetireControl.StageRequest boundRequest() {
            return boundRequest(13);
        }

        ZLinkSpotRetireControl.StageRequest boundRequest(long sessionOwnerGeneration) {
            var base = request(new byte[] {1});
            byte[] root =
                    ZLinkCanonicalActorRelocationEnvelope.encode(
                            relocationId,
                            actorId,
                            sourceSnapshot.objectGeneration(),
                            sourceSnapshot.authorityOwnerGeneration(),
                            1,
                            true,
                            new byte[] {1},
                            List.of(),
                            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of()),
                            null,
                            List.of(
                                    new systems.zlink.framework.runtime.protocol
                                            .ServiceWirePilotCodec.RelocationSessionBinding(
                                            1,
                                            RoutingId.from("session-owner").toBytes(),
                                            sessionOwnerGeneration,
                                            "session-owner-id",
                                            1,
                                            RoutingId.from("session").toBytes(),
                                            1)));
            return new ZLinkSpotRetireControl.StageRequest(
                    base.fence(),
                    base.sourceNodeRid(),
                    base.sourceNodeGeneration(),
                    base.sourceOwnerId(),
                    base.sourceOwnerLeaseGeneration(),
                    base.targetNodeRid(),
                    base.targetNodeGeneration(),
                    base.targetOwnerId(),
                    base.targetOwnerLeaseGeneration(),
                    base.meshName(),
                    base.spotId(),
                    base.stableType(),
                    base.instanceSpot(),
                    base.restoreSpotSnapshot(),
                    root,
                    base.participants(),
                    List.of());
        }

        byte[] root(byte[] applicationState) {
            return ZLinkCanonicalActorRelocationEnvelope.encode(
                    relocationId,
                    actorId,
                    sourceSnapshot.objectGeneration(),
                    sourceSnapshot.authorityOwnerGeneration(),
                    true,
                    applicationState,
                    List.of());
        }

        ZLinkSpotRetireControl.StageRequest request(byte[] applicationState) {
            return new ZLinkSpotRetireControl.StageRequest(
                    new ZLinkSpotRetireControl.Fence(relocationId, 1),
                    sourceRid,
                    11,
                    sourceOwner.ownerId(),
                    sourceOwner.leaseGeneration(),
                    targetRid,
                    12,
                    targetOwner.ownerId(),
                    targetOwner.leaseGeneration(),
                    "mesh",
                    "target-entry",
                    "actor-type",
                    false,
                    true,
                    root(applicationState),
                    List.of(
                            new ZLinkSpotRetireControl.ParticipantFence(
                                    authorityKey,
                                    1,
                                    actorId,
                                    "actor-type",
                                    true,
                                    sourceSnapshot.objectGeneration(),
                                    sourceSnapshot.authorityOwnerGeneration())),
                    List.of());
        }

        ZLinkSpotRetireControl.StageRequest requestWithAdvertisedLimit(
                byte[] applicationState, long advertisedReceiveChunkLimitBytes) {
            return new ZLinkSpotRetireControl.StageRequest(
                    new ZLinkSpotRetireControl.Fence(relocationId, 1),
                    sourceRid,
                    11,
                    sourceOwner.ownerId(),
                    sourceOwner.leaseGeneration(),
                    targetRid,
                    12,
                    targetOwner.ownerId(),
                    targetOwner.leaseGeneration(),
                    "mesh",
                    "target-entry",
                    "actor-type",
                    false,
                    true,
                    root(applicationState),
                    List.of(
                            new ZLinkSpotRetireControl.ParticipantFence(
                                    authorityKey,
                                    1,
                                    actorId,
                                    "actor-type",
                                    true,
                                    sourceSnapshot.objectGeneration(),
                                    sourceSnapshot.authorityOwnerGeneration())),
                    List.of(),
                    advertisedReceiveChunkLimitBytes);
        }
    }

    private static ZLinkAuthoritySnapshot actorAuthority(
            String actorId, RoutingId sourceRid, ZLinkLocationOwnerToken sourceOwner) {
        byte[] payload =
                new ZLinkActorAuthorityPayloadCodec()
                        .encode(
                                ZLinkActorAuthorityPayloadCodec.State.READY,
                                "actor-type",
                                actorId,
                                "source-entry",
                                1,
                                1,
                                sourceOwner.ownerId(),
                                sourceOwner.leaseGeneration(),
                                "mesh",
                                sourceRid,
                                11);
        return new ZLinkAuthoritySnapshot(
                "store-1",
                payload,
                3,
                5,
                sourceOwner.ownerId(),
                sourceOwner.leaseGeneration(),
                new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        ZLinkPlacementObjectKind.ACTOR,
                        "actor-type",
                        new ZLinkMeshNodeDescriptorKey("mesh", sourceRid),
                        11,
                        ZLinkPlacementCapacityBundle.actor(1)),
                Instant.now());
    }

    private static ZLinkSpotRetireControl.Target newRetireTarget(
            ZLinkSpotRetireControl.TargetEndpoint endpoint) throws Exception {
        var constructor =
                ZLinkSpotRetireControl.Target.class.getDeclaredConstructor(
                        ZLinkSpotRetireControl.TargetEndpoint.class);
        constructor.setAccessible(true);
        return constructor.newInstance(endpoint);
    }

    private static byte[] retireControlCodec(String name, Object... arguments) throws Exception {
        Method method =
                java.util.Arrays.stream(ZLinkSpotRetireControl.class.getDeclaredMethods())
                        .filter(
                                candidate ->
                                        candidate.getName().equals(name)
                                                && candidate.getParameterCount()
                                                        == arguments.length)
                        .findFirst()
                        .orElseThrow();
        method.setAccessible(true);
        return (byte[]) method.invoke(null, arguments);
    }

    private static Object targetAttempt(
            ZLinkCanonicalRelocationStateMachine machine, ZLinkSpotRetireControl.Fence fence)
            throws Exception {
        Field field = ZLinkCanonicalRelocationStateMachine.class.getDeclaredField("targets");
        field.setAccessible(true);
        Map<?, ?> targets = (Map<?, ?>) field.get(machine);
        Object attempt = targets.get(fence);
        return attempt != null ? attempt : targets.values().stream().findFirst().orElse(null);
    }

    private static Object targetFence(ZLinkCanonicalRelocationStateMachine machine)
            throws Exception {
        Field field = ZLinkCanonicalRelocationStateMachine.class.getDeclaredField("targets");
        field.setAccessible(true);
        return ((Map<?, ?>) field.get(machine)).keySet().stream().findFirst().orElse(null);
    }

    private static Object invokeCanonicalPublication(
            ZLinkCanonicalRelocationStateMachine machine, String name, Object... arguments)
            throws Exception {
        Method method =
                java.util.Arrays.stream(
                                ZLinkCanonicalRelocationStateMachine.class.getDeclaredMethods())
                        .filter(
                                candidate ->
                                        candidate.getName().equals(name)
                                                && candidate.getParameterCount()
                                                        == arguments.length)
                        .findFirst()
                        .orElseThrow();
        method.setAccessible(true);
        return method.invoke(machine, arguments);
    }

    private static Object attemptMember(Object attempt, String name) throws Exception {
        Method method = attempt.getClass().getDeclaredMethod(name);
        method.setAccessible(true);
        return method.invoke(attempt);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            RoutingId rid, long generation, ZLinkLocationOwnerToken owner, String entrySpotId) {
        return new ZLinkMeshNodeDescriptor(
                "mesh",
                rid,
                generation,
                1,
                "tcp://127.0.0.1:" + (7000 + generation),
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.ACTOR,
                                "actor-type",
                                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                                true,
                                0)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of(entrySpotId),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 8),
                        new ZLinkCapacityUsage(0, 0, 0),
                        List.of()),
                new ZLinkActivationConcurrency(0, 32),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                owner.ownerId(),
                owner.leaseGeneration(),
                Instant.now());
    }

    private static ZLinkInternalMeshNode node(
            RoutingId localRid,
            long generation,
            AtomicReference<ZLinkCanonicalRelocationStateMachine> peer,
            List<Integer> commands) {
        return node(
                localRid, generation, peer, commands, new AtomicReference<>(MeshNodeState.READY));
    }

    private static ZLinkInternalMeshNode node(
            RoutingId localRid,
            long generation,
            AtomicReference<ZLinkCanonicalRelocationStateMachine> peer,
            List<Integer> commands,
            AtomicReference<MeshNodeState> state) {
        return node(
                localRid,
                generation,
                peer,
                commands,
                state,
                CompletableFuture.completedFuture(null),
                new CopyOnWriteArrayList<>(),
                new CompletableFuture<>());
    }

    private static ZLinkInternalMeshNode node(
            RoutingId localRid,
            long generation,
            AtomicReference<ZLinkCanonicalRelocationStateMachine> peer,
            List<Integer> commands,
            AtomicReference<MeshNodeState> state,
            CompletableFuture<Void> sessionRoute,
            List<Map.Entry<RoutingId, Long>> securedRoutes,
            CompletableFuture<Void> stateSubmitted) {
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, args) ->
                                switch (method.getName()) {
                                    case "secureSessionOwnerRoute" -> {
                                        securedRoutes.add(
                                                Map.entry((RoutingId) args[0], (Long) args[1]));
                                        yield sessionRoute;
                                    }
                                    case "observePeerRoute" ->
                                            new ZLinkInternalMeshNode.PeerRouteObservation(1, true);
                                    case "onStateChanged", "onPeerStateChanged" -> {
                                        ((Runnable)
                                                        args[
                                                                method.getName()
                                                                                .equals(
                                                                                        "onPeerStateChanged")
                                                                        ? 1
                                                                        : 0])
                                                .run();
                                        yield (AutoCloseable) () -> {};
                                    }
                                    case "status" ->
                                            new MeshNodeStatus(
                                                    state.get(),
                                                    localRid,
                                                    "mesh",
                                                    "",
                                                    generation,
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
                                    case "sendCanonicalRelocationControl" -> {
                                        byte[] encoded = (byte[]) args[1];
                                        int command = Byte.toUnsignedInt(encoded[3]);
                                        commands.add(command);
                                        IntConsumer hook = canonicalControlHook.get();
                                        if (hook != null) {
                                            hook.accept(command);
                                        }
                                        yield peer.get()
                                                .apply(localRid, command, encoded)
                                                .whenComplete(
                                                        (ignored, failure) -> {
                                                            if (command
                                                                    == ServiceWireConstants
                                                                            .COMMAND_RELOCATION_STATE) {
                                                                if (failure == null)
                                                                    stateSubmitted.complete(null);
                                                                else
                                                                    stateSubmitted
                                                                            .completeExceptionally(
                                                                                    failure);
                                                            }
                                                        });
                                    }
                                    default ->
                                            throw new UnsupportedOperationException(
                                                    method.getName());
                                });
    }

    private static ZLinkInternalMeshNode requestReplyNode(
            RoutingId localRid,
            long generation,
            AtomicReference<ZLinkCanonicalRelocationStateMachine> peer,
            List<Integer> commands) {
        MeshNodeStatus status =
                new MeshNodeStatus(
                        MeshNodeState.READY,
                        localRid,
                        "mesh",
                        "",
                        generation,
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
        AtomicInteger prepareRequests = new AtomicInteger();
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {
                            ZLinkInternalMeshNode.class,
                            ZLinkInternalMeshNode.CanonicalRelocationPrepareRequestReplySupport
                                    .class
                        },
                        (proxy, method, args) ->
                                switch (method.getName()) {
                                    case "observePeerRoute" ->
                                            new ZLinkInternalMeshNode.PeerRouteObservation(1, true);
                                    case "onStateChanged", "onPeerStateChanged" -> {
                                        ((Runnable)
                                                        args[
                                                                method.getName()
                                                                                .equals(
                                                                                        "onPeerStateChanged")
                                                                        ? 1
                                                                        : 0])
                                                .run();
                                        yield (AutoCloseable) () -> {};
                                    }
                                    case "status" -> status;
                                    case "requestCanonicalRelocationPrepare" -> {
                                        byte[] encoded = (byte[]) args[1];
                                        commands.add(Byte.toUnsignedInt(encoded[3]));
                                        CompletionStage<byte[]> reply =
                                                peer.get()
                                                        .apply(
                                                                localRid,
                                                                1L,
                                                                ServiceWireConstants
                                                                        .COMMAND_RELOCATION_PREPARE,
                                                                encoded);
                                        if (prepareRequests.incrementAndGet() == 1) {
                                            // The request was admitted and the target owns its
                                            // stage, but the selected pair disappears before the
                                            // callback can expose READY to the source.
                                            yield reply.thenCompose(
                                                    ignored ->
                                                            CompletableFuture.failedFuture(
                                                                    new ZlinkRequestException(
                                                                            RequestResult
                                                                                    .NOT_CONNECTED)));
                                        }
                                        yield reply;
                                    }
                                    case "sendCanonicalRelocationControl" -> {
                                        byte[] encoded = (byte[]) args[1];
                                        int command = Byte.toUnsignedInt(encoded[3]);
                                        commands.add(command);
                                        yield peer.get().apply(localRid, command, encoded);
                                    }
                                    default ->
                                            throw new UnsupportedOperationException(
                                                    method.getName());
                                });
    }

    private static class CountingEndpoint implements ZLinkSpotRetireControl.TargetEndpoint {
        private final AtomicInteger staged = new AtomicInteger();
        private final AtomicInteger aborted = new AtomicInteger();
        private final AtomicInteger published = new AtomicInteger();
        private final AtomicInteger relayed = new AtomicInteger();
        private final AtomicReference<ZLinkSpotRetireControl.StageRequest> lastStaged =
                new AtomicReference<>();
        private Runnable onPublish;

        @Override
        public CompletionStage<Void> stage(ZLinkSpotRetireControl.StageRequest request) {
            staged.incrementAndGet();
            lastStaged.set(request);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> publish(ZLinkSpotRetireControl.StageRequest request) {
            published.incrementAndGet();
            if (onPublish != null) {
                onPublish.run();
            }
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> stageRelayedRecord(
                ZLinkSpotRetireControl.StageRequest request, byte[] frozenRecord) {
            assertArrayEquals(new byte[] {1, 2, 3}, frozenRecord);
            relayed.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> abort(ZLinkSpotRetireControl.StageRequest request) {
            aborted.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class FailingCleanupEndpoint extends CountingEndpoint {
        @Override
        public CompletionStage<Void> stage(ZLinkSpotRetireControl.StageRequest request) {
            return CompletableFuture.failedFuture(new IllegalStateException("pre-stage failure"));
        }

        @Override
        public CompletionStage<Void> abort(ZLinkSpotRetireControl.StageRequest request) {
            throw new IllegalStateException("target stage is unavailable");
        }
    }
}
