package systems.zlink.framework.runtime.spots;
import java.util.ArrayList;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CopyOnWriteArrayList;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

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
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

final class ZLinkUserSpotRetireTargetEndpointTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @Test
    void command46MustCloseTheExactRequestSourceFence() {
        var relay = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceRelocationWireCodec.ReplyRelay(
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceRelocationWireCodec.Operation(1, 2),
                3,
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
                6,
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceRelocationWireCodec.CoordinatorFence(
                        "target-owner", 7, RoutingId.from("target-node"),
                        8, "store-version"),
                9,
                10,
                0,
                0);
        var completion = new systems.zlink.framework.runtime.internal.locations
            .ZLinkServiceRelocationEnvelopeCodec.Completion(
                1, 2, "source-owner", 11, "source-node", 12,
                9, 10, 0, 0, 0,
                new systems.zlink.framework.runtime.internal.locations
                    .ZLinkServiceRelocationEnvelopeCodec.Payload(
                        "reply", "application/json", new byte[] {1}));
        var wrongLease = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                relay.relocation(), relay.coordinator(), relay.operation(),
                relay.replyRouteId(),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceRelocationWireCodec.RequestSourceFence(
                        "source-owner", 13, RoutingId.from("source-node"), 12),
                1);

        assertThrows(IllegalArgumentException.class, () ->
            ZLinkUserSpotRetireTargetEndpoint.validateCanonicalAck(
                relay, completion, wrongLease));
    }

    @ParameterizedTest
    @EnumSource(
        value = ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.class,
        names = {
            "APPLIED",
            "STALE",
            "SESSION_OR_BINDING_CLOSED"
        })
    void startupRecoveryRetainsRootUntilAnyExactCommand45Terminal(
        ZLinkServiceM6BWireCodec.SessionRelocationRouteResult result) {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        RoutingId sessionOwnerRid = RoutingId.from("session-owner-node");
        RoutingId sessionRid = RoutingId.from("session-a");
        long targetNodeGeneration = 17;
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        UUID relocationId = UUID.randomUUID();
        var operation = new ZLinkActorJoinOperationId(41, 73);
        var wire = new ZLinkServiceM6BWireCodec();
        var routeIntent = new ZLinkServiceM6BWireCodec
            .SessionRelocationRouteIntent(
                new ZLinkServiceM6BWireCodec.RelocationIdentity(
                    relocationId.getMostSignificantBits(),
                    relocationId.getLeastSignificantBits()),
                new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                    "source-owner",
                    12,
                    sourceRid,
                    11,
                    "actor-version-1"),
                ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
                new ZLinkServiceM6BWireCodec.ActorIdentity(actorId, 4),
                new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                    sessionOwnerRid,
                    31,
                    "session-owner",
                    32,
                    sessionRid,
                    33),
                ZLinkServiceM6BWireCodec
                    .SessionRelocationRouteAction.COMMIT,
                7,
                targetRid,
                targetNodeGeneration,
                42);
        byte[] initial = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            actorId,
            4,
            7,
            true,
            new byte[] {1},
            List.of());
        var withCompletion = ZLinkDeferredJoinCompletionAuthority
            .putRelocationCompletion(
                ZLinkServiceRelocationEnvelopeCodec.decode(initial),
                operation,
                "source-owner",
                12,
                sourceRid,
                11,
                1,
                1,
                new byte[] {9},
                wire.encodeSessionRelocationRouteIntent(routeIntent));
        byte[] completedRoot = ZLinkServiceRelocationEnvelopeCodec
            .completeDelivery(
                withCompletion,
                operation.high(),
                operation.low(),
                "source-owner",
                12,
                sourceRid,
                11,
                3)
            .canonicalBytes();
        byte[] targetProjection = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "player",
            actorId,
            "room-b",
            9,
            2,
            "source-owner",
            12,
            "mesh",
            sourceRid,
            11);
        AuthorityState authority = new AuthorityState();
        ZLinkLocationRepository authorityStore = authority.proxy();
        var roots = new InMemoryRelocationStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authorityStore, roots);
        var prepared = coordinator.prepare(
                new ZLinkAggregateRelocationCoordinator.Request(
                    relocationId,
                    1,
                    List.of(new ZLinkAggregateRelocationCoordinator.Participant(
                        authorityKey,
                        ZLinkPlacementObjectKind.ACTOR,
                        4,
                        7,
                        "actor-version-1",
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        targetProjection,
                        new byte[0])),
                    completedRoot,
                    new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                    targetNodeGeneration,
                    ZLinkPlacementCapacityBundle.actor(1),
                    new ZLinkLocationOwnerToken("target-owner", 23)),
                OPEN)
            .toCompletableFuture().join();
        coordinator.commit(prepared, OPEN).toCompletableFuture().join();
        new ZLinkDeferredJoinCompletionAuthority(authorityStore, roots)
            .markSourceCleanup(
                operation,
                new ZLinkBackendActorRef(targetRid, actorId, 4))
            .toCompletableFuture().join();
        assertFalse(authority.progress.sourceCleanupCompleted(),
            "the simulated stop occurs before the marker cleanup CAS");

        var candidate = new ZLinkRelocationStartupScanner(
                authorityStore, roots)
            .scan(OPEN)
            .toCompletableFuture().join()
            .getFirst();
        assertTrue(candidate.sourceCleanupCompleted());
        assertTrue(authority.progress.sourceCleanupCompleted(),
            "startup reconciles the marker from the exact authority publication");

        List<String> operations = new CopyOnWriteArrayList<>();
        AtomicReference<ZLinkServiceM6BWireCodec.SessionRelocationRoute>
            command44 = new AtomicReference<>();
        CompletableFuture<byte[]> command45 = new CompletableFuture<>();
        ZLinkInternalMeshNode node = restartRouteNode(
            targetRid,
            targetNodeGeneration,
            wire,
            operations,
            command44,
            command45);
        var actorStaging = new ZLinkStandaloneActorRelocationStagingOwner(
            new RestartActorBackend(operations));
        var endpoint = new ZLinkUserSpotRetireTargetEndpoint(
            targetRid,
            targetNodeGeneration,
            coordinator,
            new ZLinkUserSpotAggregateStagingOwner(new FakeStagingBackend()),
            ignored -> TestSpot.class,
            (lane, record) -> CompletableFuture.completedFuture(null),
            new ZLinkSessionRelocationPeerClient(node),
            Duration.ofSeconds(1),
            request -> coordinator.normalizeCompletedAggregate(
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
                    OPEN)
                .thenRun(() -> operations.add("normalize")),
            null,
            null,
            authorityStore,
            actorStaging);
        var machine = new ZLinkCanonicalRelocationStateMachine(
            node,
            "mesh",
            "entry",
            authorityStore,
            coordinator,
            endpoint);

        CompletableFuture<Void> recovery = machine.recoverPublished(candidate)
            .toCompletableFuture();

        assertFalse(recovery.isDone(),
            "startup keeps the durable completion referenced while ACK is pending");
        assertEquals(
            List.of("prepare-actor", "publish-actor", "open", "command44"),
            operations,
            "the Actor is Ready before command 44, but normalization waits");
        assertNotNull(command44.get());
        assertEquals(routeIntent.relocation(), command44.get().relocation());
        assertEquals(routeIntent.coordinator(), command44.get().coordinator());
        assertEquals(routeIntent.session(), command44.get().session());
        assertEquals(42, command44.get().lastAcceptedSessionSequence());
        assertTrue(ZLinkCanonicalRelocationAuthorityStateCodec.progress(
                authority.rows.get(authorityKey).payload())
            .sourceCleanupCompleted());

        var routed = command44.get();
        command45.complete(wire.encodeSessionRelocationRouted(
            new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
                routed.relocation(),
                routed.coordinator(),
                routed.actor(),
                routed.session(),
                routed.action(),
                result,
                routed.currentAuthorityOwnerGeneration(),
                result == ZLinkServiceM6BWireCodec
                        .SessionRelocationRouteResult.APPLIED
                    ? routed.lastAcceptedSessionSequence()
                    : 0)));
        recovery.join();

        assertEquals(
            List.of(
                "prepare-actor",
                "publish-actor",
                "open",
                "command44",
                "normalize"),
            operations);
        assertTrue(new ZLinkActorAuthorityPayloadCodec().decode(
                authority.rows.get(authorityKey).payload()).isPresent(),
            "only terminal command 45 releases the authority root pointer");
        assertTrue(authority.progress == null,
            "normalization removes the durable recovery marker after ACK");
    }

    @Test
    void startupRetriesExactRetainedSourceAbortBeforeDeletingItsRoot()
        throws InterruptedException {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        RoutingId sessionOwnerRid = RoutingId.from("session-owner-node");
        RoutingId sessionRid = RoutingId.from("session-a");
        long targetNodeGeneration = 17;
        String actorId = "actor-a";
        UUID relocationId = UUID.randomUUID();
        var wire = new ZLinkServiceM6BWireCodec();
        var intent = new ZLinkServiceM6BWireCodec
            .SessionRelocationRouteIntent(
                new ZLinkServiceM6BWireCodec.RelocationIdentity(
                    relocationId.getMostSignificantBits(),
                    relocationId.getLeastSignificantBits()),
                new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                    "source-owner", 12, sourceRid, 11, "actor-version-1"),
                ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
                new ZLinkServiceM6BWireCodec.ActorIdentity(actorId, 4),
                new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                    sessionOwnerRid,
                    31,
                    "session-owner",
                    32,
                    sessionRid,
                    33),
                ZLinkServiceM6BWireCodec
                    .SessionRelocationRouteAction.COMMIT,
                7,
                targetRid,
                targetNodeGeneration,
                42);
        var root = ZLinkDeferredJoinCompletionAuthority
            .putRelocationCompletion(
                ZLinkServiceRelocationEnvelopeCodec.decode(
                    ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId,
                        actorId,
                        4,
                        7,
                        false,
                        new byte[0],
                        List.of())),
                new ZLinkActorJoinOperationId(41, 73),
                "source-owner",
                12,
                sourceRid,
                11,
                1,
                1,
                new byte[] {9},
                wire.encodeSessionRelocationRouteIntent(intent))
            .canonicalBytes();
        byte[] targetProjection = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "player",
            actorId,
            "room-b",
            9,
            2,
            "source-owner",
            12,
            "mesh",
            sourceRid,
            11);
        AuthorityState authority = new AuthorityState();
        ZLinkLocationRepository locations = authority.proxy();
        var roots = new InMemoryRelocationStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            locations, roots);
        var prepared = coordinator.prepare(
                new ZLinkAggregateRelocationCoordinator.Request(
                    relocationId,
                    1,
                    List.of(new ZLinkAggregateRelocationCoordinator.Participant(
                        ZLinkAuthorityKeyCodec.actor(actorId),
                        ZLinkPlacementObjectKind.ACTOR,
                        4,
                        7,
                        "actor-version-1",
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        targetProjection,
                        new byte[0])),
                    root,
                    new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                    targetNodeGeneration,
                    ZLinkPlacementCapacityBundle.actor(1),
                    new ZLinkLocationOwnerToken("target-owner", 23)),
                OPEN)
            .toCompletableFuture().join();
        locations.retainAggregateAbort(prepared.fence(), OPEN)
            .toCompletableFuture().join();
        var candidate = new ZLinkRelocationStartupScanner(locations, roots)
            .scanRetainedSessionAborts(OPEN)
            .toCompletableFuture().join().getFirst();

        AtomicInteger submissions = new AtomicInteger();
        AtomicReference<ZLinkServiceM6BWireCodec.SessionRelocationRoute> abort =
            new AtomicReference<>();
        CompletableFuture<byte[]> terminal = new CompletableFuture<>();
        MeshNodeStatus status = new MeshNodeStatus(
            MeshNodeState.READY,
            sourceRid,
            "mesh",
            "",
            11,
            1, 0, 1, 1, 0, 0, 0, 0, 0, 0);
        ZLinkInternalMeshNode node = (ZLinkInternalMeshNode)
            Proxy.newProxyInstance(
                ZLinkInternalMeshNode.class.getClassLoader(),
                new Class<?>[] {ZLinkInternalMeshNode.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "status" -> status;
                    case "requestSessionRelocationRoute" -> {
                        abort.set(wire.decodeSessionRelocationRoute(
                            (byte[]) arguments[1]));
                        yield submissions.incrementAndGet() == 1
                            ? CompletableFuture.failedFuture(
                                new IllegalStateException("link unavailable"))
                            : terminal;
                    }
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        var endpoint = new ZLinkUserSpotRetireTargetEndpoint(
            sourceRid,
            11,
            coordinator,
            new ZLinkUserSpotAggregateStagingOwner(new FakeStagingBackend()),
            ignored -> TestSpot.class,
            (lane, record) -> CompletableFuture.completedFuture(null),
            new ZLinkSessionRelocationPeerClient(node),
            Duration.ofSeconds(3),
            ignored -> CompletableFuture.completedFuture(null));
        var machine = new ZLinkCanonicalRelocationStateMachine(
            node,
            "mesh",
            "entry",
            locations,
            coordinator,
            endpoint);

        CompletableFuture<Void> recovery = machine
            .recoverRetainedSessionAbort(candidate)
            .toCompletableFuture();
        long retryDeadline = System.nanoTime()
            + Duration.ofSeconds(2).toNanos();
        while (submissions.get() < 2 && System.nanoTime() < retryDeadline) {
            Thread.sleep(10);
        }

        assertEquals(2, submissions.get());
        assertFalse(recovery.isDone());
        assertEquals(1, locations.listRetainedAggregateAborts(OPEN)
            .toCompletableFuture().join().size(),
            "a failed command 45 attempt keeps the durable recovery owner");
        assertTrue(roots.get(prepared.stored().reference(), OPEN)
            .toCompletableFuture().join() instanceof ZLinkRelocationFound);
        assertEquals(ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            abort.get().senderRole());
        assertEquals(
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            abort.get().action());
        assertEquals(0, abort.get().lastAcceptedSessionSequence());

        terminal.complete(wire.encodeSessionRelocationRouted(
            new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
                abort.get().relocation(),
                abort.get().coordinator(),
                abort.get().actor(),
                abort.get().session(),
                abort.get().action(),
                ZLinkServiceM6BWireCodec
                    .SessionRelocationRouteResult.APPLIED,
                abort.get().currentAuthorityOwnerGeneration(),
                42)));
        recovery.join();

        assertTrue(locations.listRetainedAggregateAborts(OPEN)
            .toCompletableFuture().join().isEmpty());
        assertTrue(roots.get(prepared.stored().reference(), OPEN)
            .toCompletableFuture().join() instanceof ZLinkRelocationMissing,
            "only the exact command 45 terminal releases the durable root");
    }

    @Test
    void targetStaysInvisibleAndStagesPostCutRequestUntilExactRootIsPublished() {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        long targetNodeGeneration = 17;
        String spotId = "room-a";
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.spot(spotId);
        String actorAuthorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        AuthorityState authority = new AuthorityState();
        ZLinkLocationRepository authorityStore = authority.proxy();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authorityStore,
            new InMemoryRelocationStore());
        var envelope = new ZLinkUserSpotAggregateStagingOwner.Request(
            TestSpot.class,
            "room",
            spotId,
            3,
            new byte[] {1},
            true,
            new byte[0],
            List.of(new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                actorId,
                "player",
                new byte[0],
                false,
                new ZLinkBackendActorRef(targetRid, actorId, 4))),
            Map.of("spot", List.of(
                new ZLinkAsyncSerialQueue.QueuedRecord(
                    1, acceptedSpotRecord(spotId, 1)))));
        UUID aggregateId = UUID.randomUUID();
        var participants = List.of(
            new ZLinkSpotRetireControl.ParticipantFence(
                actorAuthorityKey,
                1,
                actorId,
                "player",
                false,
                4,
                7),
            new ZLinkSpotRetireControl.ParticipantFence(
                authorityKey,
                2,
                spotId,
                "room",
                true,
                3,
                5));
        var prepared = coordinator.stageRoot(
                new ZLinkAggregateRelocationCoordinator.Request(
                    aggregateId,
                    1,
                    List.of(
                        new ZLinkAggregateRelocationCoordinator.Participant(
                            actorAuthorityKey,
                            ZLinkPlacementObjectKind.ACTOR,
                            4,
                            7,
                            "actor-version-1",
                            ZLinkAuthorityGenerationTransition.NEW_OWNER,
                            sourceAuthority(actorId, sourceRid),
                            new byte[0]),
                        new ZLinkAggregateRelocationCoordinator.Participant(
                            authorityKey,
                            ZLinkPlacementObjectKind.USER_SPOT,
                            3,
                            5,
                            "version-1",
                            ZLinkAuthorityGenerationTransition.NEW_OWNER,
                            sourceAuthority(spotId, sourceRid),
                            new byte[0])),
                    ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                        envelope, aggregateId, 5, participants),
                    new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                    targetNodeGeneration,
                    new ZLinkPlacementCapacityBundle(
                        1,
                        1,
                        Optional.of(new ZLinkSpotTypeCapacityDelta(
                            ZLinkPlacementObjectKind.USER_SPOT,
                            "room",
                            1))),
                    new ZLinkLocationOwnerToken("target-owner", 23)),
                OPEN)
            .toCompletableFuture().join();
        FakeStagingBackend backend = new FakeStagingBackend();
        var staging = new ZLinkUserSpotAggregateStagingOwner(backend);
        RoutingId sessionOwnerRid = RoutingId.from("session-owner");
        RoutingId sessionRid = RoutingId.from("session-a");
        var wire = new ZLinkServiceM6BWireCodec();
        ZLinkInternalMeshNode peer = sessionRouteNode(wire, backend.operations);
        var endpoint = new ZLinkUserSpotRetireTargetEndpoint(
            targetRid,
            targetNodeGeneration,
            coordinator,
            staging,
            ignored -> TestSpot.class,
            (lane, record) -> {
                assertTrue(backend.live.isEmpty(),
                    "accepted replay must finish before local publication");
                backend.operations.add("replay:" + record.sequence());
                return CompletableFuture.completedFuture(null);
            },
            new ZLinkSessionRelocationPeerClient(peer),
            Duration.ofSeconds(1),
            ignored -> {
                backend.operations.add("normalize");
                return CompletableFuture.completedFuture(null);
            });
        var request = new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(aggregateId, 1),
            sourceRid,
            11,
            "source-owner",
            12,
            targetRid,
            targetNodeGeneration,
            "target-owner",
            23,
            "mesh",
            spotId,
            "room",
            false,
            true,
            prepared.stored().reference(),
            prepared.stored().checksumCrc32c(),
            participants,
            List.of(new ZLinkSpotRetireControl.SessionRouteFence(
                actorId,
                4,
                7,
                "actor-version-1",
                sessionOwnerRid,
                31,
                "session-owner-id",
                32,
                sessionRid,
                33,
                34)));

        endpoint.stage(request).toCompletableFuture().join();
        assertTrue(backend.live.isEmpty());
        assertThrows(IllegalStateException.class, () ->
            endpoint.releaseRecoveryPointer(
                request,
                ZLinkUserSpotRetireTargetEndpoint.RecoveryReleaseEvidence
                    .REPLY_RELAY_ACK));

        var finalEnvelope = new ZLinkUserSpotAggregateStagingOwner.Request(
            TestSpot.class,
            "room",
            spotId,
            3,
            new byte[] {1},
            true,
            new byte[0],
            List.of(new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                actorId,
                "player",
                new byte[0],
                false,
                new ZLinkBackendActorRef(targetRid, actorId, 4))),
            Map.of("spot", List.of(
                new ZLinkAsyncSerialQueue.QueuedRecord(
                    1, acceptedSpotRecord(spotId, 1)),
                new ZLinkAsyncSerialQueue.QueuedRecord(
                    2, acceptedSpotRequest(spotId, 2)))));
        var finalPrepared = coordinator.prepare(
                new ZLinkAggregateRelocationCoordinator.Request(
                    aggregateId,
                    1,
                    List.of(
                        new ZLinkAggregateRelocationCoordinator.Participant(
                            actorAuthorityKey,
                            ZLinkPlacementObjectKind.ACTOR,
                            4,
                            7,
                            "actor-version-1",
                            ZLinkAuthorityGenerationTransition.NEW_OWNER,
                            sourceAuthority(actorId, sourceRid),
                            new byte[0]),
                        new ZLinkAggregateRelocationCoordinator.Participant(
                            authorityKey,
                            ZLinkPlacementObjectKind.USER_SPOT,
                            3,
                            5,
                            "version-1",
                            ZLinkAuthorityGenerationTransition.NEW_OWNER,
                            sourceAuthority(spotId, sourceRid),
                            new byte[0])),
                    ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                        finalEnvelope, aggregateId, 5, participants),
                    new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                    targetNodeGeneration,
                    new ZLinkPlacementCapacityBundle(
                        1,
                        1,
                        Optional.of(new ZLinkSpotTypeCapacityDelta(
                            ZLinkPlacementObjectKind.USER_SPOT,
                            "room",
                            1))),
                    new ZLinkLocationOwnerToken("target-owner", 23)),
                OPEN)
            .toCompletableFuture().join();
        var activated = coordinator.commit(finalPrepared, OPEN)
            .toCompletableFuture().join();
        AtomicInteger relays = new AtomicInteger();
        FakeStagingBackend recoveredBackend = new FakeStagingBackend();
        var recoveredStaging =
            new ZLinkUserSpotAggregateStagingOwner(recoveredBackend);
        ZLinkInternalMeshNode recoveryPeer = recoveryNode(
            wire, recoveredBackend.operations, relays);
        var inboundPermits = new ZLinkRelocationPermitPool(
            new ZLinkLocationOptions());
        var recoveredEndpoint = new ZLinkUserSpotRetireTargetEndpoint(
            targetRid,
            targetNodeGeneration,
            coordinator,
            recoveredStaging,
            ignored -> TestSpot.class,
            (lane, record) -> CompletableFuture.failedFuture(
                new AssertionError(
                    "restart recovery must use the production replayer")),
            new ZLinkSessionRelocationPeerClient(recoveryPeer),
            Duration.ofSeconds(1),
            ignored -> {
                recoveredBackend.operations.add("normalize");
                return CompletableFuture.completedFuture(null);
            },
            null,
            ZLinkSpotRetireControl.client(recoveryPeer),
            authorityStore,
            null,
            inboundPermits);
        recoveredEndpoint.stage(request).toCompletableFuture().join();
        assertEquals(1, inboundPermits.snapshot().inboundUnits());
        assertEquals(1, inboundPermits.snapshot().restoreCallbacks());
        assertTrue(inboundPermits.snapshot().payloadBytes() > 0);
        List<byte[]> stagedReplies = new CopyOnWriteArrayList<>();
        AtomicInteger stagedFailures = new AtomicInteger();
        byte[] stagedRecord = acceptedSpotRequest(spotId, 3);
        var stagedHeader = new ZLinkServiceM6BWireCodec.SpotMessage(
            true,
            0,
            43L,
            301,
            303,
            1,
            "source",
            new ZLinkServiceM6BWireCodec.SpotRouteFence(
                spotId,
                3,
                targetRid,
                targetNodeGeneration,
                6,
                23));
        Message wrongSourcePart = Message.from("wrong-source");
        assertFalse(recoveredEndpoint.handleSpot(
            new ZLinkInternalMeshNode.PeerAuthorityFence(
                sourceRid, 11, "wrong-owner", 12),
            stagedHeader,
            new byte[0],
            () -> stagedRecord,
            stagedRecord.length,
            List.of(wrongSourcePart),
            null,
            null,
            null,
            ignored -> stagedFailures.incrementAndGet()));
        wrongSourcePart.close();
        assertTrue(recoveredEndpoint.handleSpot(
            new ZLinkInternalMeshNode.PeerAuthorityFence(
                sourceRid, 11, "source-owner", 12),
            stagedHeader,
            new byte[0],
            () -> stagedRecord,
            stagedRecord.length,
            List.of(Message.from("target-staged-request")),
            null,
            null,
            reply -> {
                stagedReplies.add(reply.getFirst().toByteArray());
                reply.forEach(Message::close);
            },
            ignored -> stagedFailures.incrementAndGet()));
        assertTrue(stagedReplies.isEmpty());
        assertEquals(0, stagedFailures.get());
        recoveredEndpoint.publish(request).toCompletableFuture().join();

        assertEquals(List.of(actorId, "spot"), recoveredBackend.live);
        assertEquals(List.of(
            "prepare", "restore", "prepare-actor", "replay-send:1",
            "replay-request:2",
            "publish-actor", "publish", "replay-request:3",
            "complete-actor", "timers"),
            recoveredBackend.operations,
            "admission (timers, staged terminal) opens at publish with no"
                + " command 35 delivered");
        assertEquals(1, stagedReplies.size());
        assertArrayEquals(new byte[] {9}, stagedReplies.getFirst());
        assertEquals(0, stagedFailures.get());
        assertEquals(2, relays.get(),
            "command 33 is retried until command 46 reports terminal");
        assertThrows(CompletionException.class, () ->
            recoveredEndpoint.finalizeAfterCompletion(request)
                .toCompletableFuture()
                .join());
        assertTrue(recoveredBackend.operations.stream()
                .noneMatch("command44"::equals),
            "command 44 must not run before Completed authority CAS");
        assertThrows(IllegalStateException.class, () ->
            recoveredEndpoint.releaseRecoveryPointer(
                request,
                ZLinkUserSpotRetireTargetEndpoint.RecoveryReleaseEvidence
                    .REPLY_RELAY_ACK));

        byte[] completedRoot = ZLinkCanonicalUserSpotRelocationEnvelope.encode(
            finalEnvelope, aggregateId, 5, participants);
        coordinator.completeSourceCleanup(activated, completedRoot, OPEN)
            .toCompletableFuture().join();
        recoveredEndpoint.finalizeAfterCompletion(request)
            .toCompletableFuture().join();
        assertEquals(0, inboundPermits.snapshot().inboundUnits());
        assertEquals(0, inboundPermits.snapshot().restoreCallbacks());
        assertEquals(0, inboundPermits.snapshot().payloadBytes());

        assertEquals(List.of(
            "prepare", "restore", "prepare-actor", "replay-send:1",
            "replay-request:2",
            "publish-actor", "publish", "replay-request:3",
            "complete-actor", "timers",
            "command44", "normalize"),
            recoveredBackend.operations,
            "a failed finalize leaves admission open and the retried"
                + " command 35 converges route switch and normalization");
        assertEquals(0, recoveredEndpoint.activeRecoveryPointerCount(),
            "completed root with no pending relay releases the target slot");
    }

    @Test
    void crossNodeReplyRelayIsAddressedToTheRelocationSourceNode() {
        //  The frozen record's request-source fence names "journal-node"
        //  while the relocation source (the node that owns the landed reply
        //  capability) is "source-node". Command 33 must be addressed to the
        //  relocation source, not to the request-source fence.
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId journalRid = RoutingId.from("journal-node");
        RoutingId targetRid = RoutingId.from("target-node");
        long targetNodeGeneration = 17;
        String spotId = "room-a";
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.spot(spotId);
        String actorAuthorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        AuthorityState authority = new AuthorityState();
        ZLinkLocationRepository authorityStore = authority.proxy();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authorityStore,
            new InMemoryRelocationStore());
        UUID aggregateId = UUID.randomUUID();
        var participants = List.of(
            new ZLinkSpotRetireControl.ParticipantFence(
                actorAuthorityKey, 1, actorId, "player", false, 4, 7),
            new ZLinkSpotRetireControl.ParticipantFence(
                authorityKey, 2, spotId, "room", true, 3, 5));
        var finalEnvelope = new ZLinkUserSpotAggregateStagingOwner.Request(
            TestSpot.class,
            "room",
            spotId,
            3,
            new byte[] {1},
            true,
            new byte[0],
            List.of(new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                actorId,
                "player",
                new byte[0],
                false,
                new ZLinkBackendActorRef(targetRid, actorId, 4))),
            Map.of("spot", List.of(
                new ZLinkAsyncSerialQueue.QueuedRecord(
                    1, acceptedSpotRequest(spotId, 2)))));
        var storeParticipants = List.of(
            new ZLinkAggregateRelocationCoordinator.Participant(
                actorAuthorityKey,
                ZLinkPlacementObjectKind.ACTOR,
                4,
                7,
                "actor-version-1",
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                sourceAuthority(actorId, sourceRid),
                new byte[0]),
            new ZLinkAggregateRelocationCoordinator.Participant(
                authorityKey,
                ZLinkPlacementObjectKind.USER_SPOT,
                3,
                5,
                "version-1",
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                sourceAuthority(spotId, sourceRid),
                new byte[0]));
        var storeRequest = new ZLinkAggregateRelocationCoordinator.Request(
            aggregateId,
            1,
            storeParticipants,
            ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                finalEnvelope, aggregateId, 5, participants),
            new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
            targetNodeGeneration,
            new ZLinkPlacementCapacityBundle(
                1,
                1,
                Optional.of(new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "room",
                    1))),
            new ZLinkLocationOwnerToken("target-owner", 23));
        var staged = coordinator.stageRoot(storeRequest, OPEN)
            .toCompletableFuture().join();
        var finalPrepared = coordinator.prepare(storeRequest, OPEN)
            .toCompletableFuture().join();
        coordinator.commit(finalPrepared, OPEN)
            .toCompletableFuture().join();

        AtomicInteger relays = new AtomicInteger();
        List<RoutingId> relayDestinations =
            new CopyOnWriteArrayList<>();
        List<systems.zlink.framework.runtime.internal.service
            .ZLinkServiceRelocationWireCodec.RequestSourceFence>
            relayExpectedSources =
                new CopyOnWriteArrayList<>();
        FakeStagingBackend backend = new FakeStagingBackend();
        var staging = new ZLinkUserSpotAggregateStagingOwner(backend);
        var wire = new ZLinkServiceM6BWireCodec();
        ZLinkInternalMeshNode peer = recoveryNode(
            wire,
            backend.operations,
            relays,
            relayDestinations,
            relayExpectedSources);
        var endpoint = new ZLinkUserSpotRetireTargetEndpoint(
            targetRid,
            targetNodeGeneration,
            coordinator,
            staging,
            ignored -> TestSpot.class,
            (lane, record) -> CompletableFuture.failedFuture(
                new AssertionError(
                    "canonical recovery must use the production replayer")),
            new ZLinkSessionRelocationPeerClient(peer),
            Duration.ofSeconds(1),
            ignored -> CompletableFuture.completedFuture(null),
            null,
            ZLinkSpotRetireControl.client(peer),
            authorityStore,
            null,
            new ZLinkRelocationPermitPool(new ZLinkLocationOptions()));
        var request = new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(aggregateId, 1),
            sourceRid,
            11,
            "source-owner",
            12,
            targetRid,
            targetNodeGeneration,
            "target-owner",
            23,
            "mesh",
            spotId,
            "room",
            false,
            true,
            staged.stored().reference(),
            staged.stored().checksumCrc32c(),
            participants);

        endpoint.stage(request).toCompletableFuture().join();
        endpoint.publish(request).toCompletableFuture().join();

        assertEquals(2, relays.get(),
            "command 33 is retried until command 46 reports terminal");
        assertEquals(List.of(sourceRid, sourceRid), relayDestinations,
            "command 33 must be addressed to the relocation source node");
        assertEquals(2, relayExpectedSources.size());
        relayExpectedSources.forEach(expected -> assertEquals(
            journalRid,
            expected.nodeRid(),
            "the expected source keeps the request-source lease fence"));
    }

    private static byte[] acceptedSpotRecord(String spotId, int value) {
        return ZLinkAcceptedJournalTestRecords.spot(
            "source", spotId, 0, "packet-" + value, Map.of(),
            new byte[] {(byte) value});
    }

    private static byte[] acceptedSpotRequest(String spotId, int value) {
        return ZLinkAcceptedJournalTestRecords.spot(
            "source", spotId, 40 + value, "packet-" + value, Map.of(),
            new byte[] {(byte) value});
    }

    private static byte[] sourceAuthority(String spotId, RoutingId sourceRid) {
        return new ZLinkServiceAuthorityPayloadCodec().encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "room", spotId, "source-owner", 12, "mesh", sourceRid, 11);
    }

    private static ZLinkInternalMeshNode sessionRouteNode(
        ZLinkServiceM6BWireCodec wire,
        List<String> operations) {
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("requestSessionRelocationRoute")) {
                    var command = wire.decodeSessionRelocationRoute(
                        (byte[]) arguments[1]);
                    operations.add("command44");
                    return CompletableFuture.completedFuture(
                        wire.encodeSessionRelocationRouted(
                            new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
                                command.relocation(),
                                command.coordinator(),
                                command.actor(),
                                command.session(),
                                command.action(),
                                ZLinkServiceM6BWireCodec
                                    .SessionRelocationRouteResult.APPLIED,
                                command.currentAuthorityOwnerGeneration(),
                                command.lastAcceptedSessionSequence())));
                }
                if (method.getDeclaringClass() == Object.class) {
                    return method.invoke(proxy, arguments);
                }
                throw new UnsupportedOperationException(method.getName());
            });
    }

    private static ZLinkInternalMeshNode restartRouteNode(
        RoutingId localRid,
        long generation,
        ZLinkServiceM6BWireCodec wire,
        List<String> operations,
        AtomicReference<ZLinkServiceM6BWireCodec.SessionRelocationRoute>
            command44,
        CompletableFuture<byte[]> command45) {
        MeshNodeStatus status = new MeshNodeStatus(
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
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "status" -> status;
                case "requestSessionRelocationRoute" -> {
                    command44.set(wire.decodeSessionRelocationRoute(
                        (byte[]) arguments[1]));
                    operations.add("command44");
                    yield command45;
                }
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static ZLinkInternalMeshNode recoveryNode(
        ZLinkServiceM6BWireCodec wire,
        List<String> operations,
        AtomicInteger relays) {
        return recoveryNode(
            wire,
            operations,
            relays,
            new CopyOnWriteArrayList<>(),
            new CopyOnWriteArrayList<>());
    }

    private static ZLinkInternalMeshNode recoveryNode(
        ZLinkServiceM6BWireCodec wire,
        List<String> operations,
        AtomicInteger relays,
        List<RoutingId> relayDestinations,
        List<systems.zlink.framework.runtime.internal.service
            .ZLinkServiceRelocationWireCodec.RequestSourceFence>
            relayExpectedSources) {
        var relocationWire =
            new systems.zlink.framework.runtime.internal.service
                .ZLinkServiceRelocationWireCodec();
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals(
                    "requestSessionRelocationRoute")) {
                    var command = wire.decodeSessionRelocationRoute(
                        (byte[]) arguments[1]);
                    operations.add("command44");
                    return CompletableFuture.completedFuture(
                        wire.encodeSessionRelocationRouted(
                            new ZLinkServiceM6BWireCodec
                                .SessionRelocationRouted(
                                    command.relocation(),
                                    command.coordinator(),
                                    command.actor(),
                                    command.session(),
                                    command.action(),
                                    ZLinkServiceM6BWireCodec
                                        .SessionRelocationRouteResult.APPLIED,
                                    command
                                        .currentAuthorityOwnerGeneration(),
                                    command
                                        .lastAcceptedSessionSequence())));
                }
                if (method.getName().equals(
                    "requestRelocationReplyRelay")) {
                    relayDestinations.add((RoutingId) arguments[0]);
                    var expected = (systems.zlink.framework.runtime.internal
                        .service.ZLinkServiceRelocationWireCodec
                            .RequestSourceFence) arguments[1];
                    relayExpectedSources.add(expected);
                    var relay = relocationWire.decodeReplyRelay(
                        (byte[]) arguments[2]);
                    int status = relays.incrementAndGet() == 1 ? 1 : 2;
                    return CompletableFuture.completedFuture(
                        relocationWire.encodeReplyRelayAck(
                            new systems.zlink.framework.runtime.internal
                                .service.ZLinkServiceRelocationWireCodec
                                    .ReplyRelayAck(
                                        relay.relocation(),
                                        relay.coordinator(),
                                        relay.operation(),
                                        relay.replyRouteId(),
                                        expected,
                                        status)));
                }
                if (method.getDeclaringClass() == Object.class) {
                    return method.invoke(proxy, arguments);
                }
                throw new UnsupportedOperationException(method.getName());
            });
    }

    private static final class AuthorityState {
        private final Map<String, ZLinkAuthoritySnapshot> rows =
            new ConcurrentHashMap<>();
        private ZLinkAggregatePrepareRequest prepared;
        private ZLinkAggregateProgress progress;
        private String progressStoreVersion;
        private ZLinkAggregateAbortRecoverySnapshot retainedAbort;
        private ZLinkAggregateAbortCleanupSnapshot terminalAbort;

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
                            (String) arguments[0],
                            null) == null
                            ? new ZLinkAuthorityMissing(Instant.now())
                            : rows.get((String) arguments[0]));
                    case "compareExchange" -> compareExchange(
                        (String) arguments[0],
                        (ZLinkAuthorityExpectation) arguments[1],
                        (ZLinkAuthorityMutation) arguments[2]);
                    case "readAggregateProgress" -> readAggregateProgress(
                        (ZLinkAggregateFence) arguments[0]);
                    case "compareExchangeAggregateProgress" ->
                        compareExchangeAggregateProgress(
                            (ZLinkAggregateFence) arguments[0],
                            (String) arguments[1],
                            (ZLinkAggregateProgress) arguments[2]);
                    case "listAggregateProgress" -> listAggregateProgress();
                    case "removeAggregateProgress" ->
                        removeAggregateProgress(
                            (ZLinkAggregateFence) arguments[0],
                            (String) arguments[1]);
                    case "retainAggregateAbort" -> retainAggregateAbort(
                        (ZLinkAggregateFence) arguments[0]);
                    case "listRetainedAggregateAborts" ->
                        CompletableFuture.completedFuture(
                            retainedAbort == null
                                ? List.of() : List.of(retainedAbort));
                    case "markAggregateAbortTerminal" ->
                        markAggregateAbortTerminal(
                            (ZLinkAggregateFence) arguments[0],
                            (String) arguments[1],
                            (String) arguments[2],
                            (long) arguments[3]);
                    case "listTerminalAggregateAborts" ->
                        CompletableFuture.completedFuture(
                            terminalAbort == null
                                ? List.of() : List.of(terminalAbort));
                    case "cleanupTerminalAggregateAbortInventory" ->
                        CompletableFuture.completedFuture(true);
                    case "removeTerminalAggregateAbort" ->
                        removeTerminalAggregateAbort(
                            (ZLinkAggregateAbortCleanupSnapshot) arguments[0]);
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        }

        private CompletionStage<ZLinkAggregatePrepareResult> prepare(
            ZLinkAggregatePrepareRequest request) {
            prepared = request;
            return CompletableFuture.completedFuture(
                new ZLinkAggregatePrepared(new ZLinkAggregateFence(
                    request.aggregateId(),
                    request.aggregateGeneration())));
        }

        private CompletionStage<ZLinkAggregateCommitResult> commit(
            ZLinkAggregateFence fence) {
            if (retainedAbort != null
                || prepared == null
                || !prepared.aggregateId().equals(fence.aggregateId())
                || prepared.aggregateGeneration()
                    != fence.aggregateGeneration()) {
                return CompletableFuture.completedFuture(
                    ZLinkAggregateCommitResult.STALE);
            }
            for (var participant : prepared.participants()) {
                boolean actor = participant.authorityKey().contains(":a:");
                rows.put(participant.authorityKey(), new ZLinkAuthoritySnapshot(
                    "version-2",
                    participant.authorityPayload(),
                    actor ? 4 : 3,
                    actor ? 8 : 6,
                    prepared.targetOwner().ownerId(),
                    prepared.targetOwner().leaseGeneration(),
                    new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        actor
                            ? ZLinkPlacementObjectKind.ACTOR
                            : ZLinkPlacementObjectKind.USER_SPOT,
                        actor ? "player" : "room",
                        prepared.targetDescriptor(),
                        prepared.targetDescriptorLifecycleGeneration(),
                        prepared.capacityBundle()),
                    Instant.now()));
            }
            progress = ZLinkCanonicalRelocationAuthorityStateCodec.progress(
                prepared.participants().getFirst().authorityPayload());
            progressStoreVersion = "aggregate-commit";
            return CompletableFuture.completedFuture(
                ZLinkAggregateCommitResult.COMMITTED);
        }

        private CompletionStage<ZLinkAggregateAbortRecoverySnapshot>
            retainAggregateAbort(ZLinkAggregateFence fence) {
            if (prepared == null
                || !prepared.aggregateId().equals(fence.aggregateId())
                || prepared.aggregateGeneration()
                    != fence.aggregateGeneration()) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException("aggregate is not prepared"));
            }
            if (retainedAbort == null) {
                retainedAbort = new ZLinkAggregateAbortRecoverySnapshot(
                    fence, "aggregate-aborted", prepared);
            }
            return CompletableFuture.completedFuture(retainedAbort);
        }

        private CompletionStage<Optional<ZLinkAggregateAbortCleanupSnapshot>>
            markAggregateAbortTerminal(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            String reference,
            long checksumCrc32c) {
            if (terminalAbort != null) {
                return CompletableFuture.completedFuture(
                    Optional.of(terminalAbort));
            }
            if (retainedAbort == null
                || !retainedAbort.fence().equals(fence)
                || !retainedAbort.storeVersion().equals(expectedStoreVersion)) {
                return CompletableFuture.completedFuture(Optional.empty());
            }
            retainedAbort = null;
            terminalAbort = new ZLinkAggregateAbortCleanupSnapshot(
                fence,
                "aggregate-terminal",
                reference,
                checksumCrc32c);
            return CompletableFuture.completedFuture(Optional.of(terminalAbort));
        }

        private CompletionStage<Boolean> removeTerminalAggregateAbort(
            ZLinkAggregateAbortCleanupSnapshot cleanup) {
            if (!cleanup.equals(terminalAbort)) {
                return CompletableFuture.completedFuture(false);
            }
            terminalAbort = null;
            prepared = null;
            return CompletableFuture.completedFuture(true);
        }

        private CompletionStage<Optional<ZLinkAggregateProgressSnapshot>>
            readAggregateProgress(ZLinkAggregateFence fence) {
            return CompletableFuture.completedFuture(
                progress == null
                    ? Optional.empty()
                    : Optional.of(progressSnapshot(fence)));
        }

        private CompletionStage<ZLinkAggregateProgressWriteResult>
            compareExchangeAggregateProgress(
                ZLinkAggregateFence fence,
                String expectedStoreVersion,
                ZLinkAggregateProgress next) {
            if (progress == null
                || !progressStoreVersion.equals(expectedStoreVersion)) {
                return CompletableFuture.completedFuture(
                    new ZLinkAggregateProgressConflict());
            }
            progress = next;
            progressStoreVersion = "aggregate-progress";
            return CompletableFuture.completedFuture(
                new ZLinkAggregateProgressStored(progressSnapshot(fence)));
        }

        private CompletionStage<List<ZLinkAggregateProgressSnapshot>>
            listAggregateProgress() {
            return CompletableFuture.completedFuture(
                progress == null
                    ? List.of()
                    : List.of(progressSnapshot(new ZLinkAggregateFence(
                        prepared.aggregateId(),
                        prepared.aggregateGeneration()))));
        }

        private CompletionStage<Boolean> removeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion) {
            if (progress == null
                || !progressStoreVersion.equals(expectedStoreVersion)) {
                return CompletableFuture.completedFuture(false);
            }
            progress = null;
            progressStoreVersion = null;
            return CompletableFuture.completedFuture(true);
        }

        private ZLinkAggregateProgressSnapshot progressSnapshot(
            ZLinkAggregateFence fence) {
            return new ZLinkAggregateProgressSnapshot(
                fence,
                progressStoreVersion,
                prepared,
                progress);
        }

        private CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key,
            ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation) {
            ZLinkAuthoritySnapshot current = rows.get(key);
            if (!(expectation instanceof ZLinkAuthorityExpectFound found)
                || current == null
                || !current.storeVersion().equals(found.storeVersion())
                || !(mutation instanceof ZLinkAuthorityPut put)
                || put.generationTransition()
                    != ZLinkAuthorityGenerationTransition.PRESERVE) {
                return CompletableFuture.completedFuture(
                    new ZLinkAuthorityConflict(current == null
                        ? new ZLinkAuthorityMissing(Instant.now())
                        : current));
            }
            ZLinkAuthoritySnapshot stored = new ZLinkAuthoritySnapshot(
                "cas-" + key,
                put.payload(),
                current.objectGeneration(),
                current.authorityOwnerGeneration(),
                current.ownerId(),
                current.ownerLeaseGeneration(),
                current.allocation(),
                Instant.now());
            rows.put(key, stored);
            return CompletableFuture.completedFuture(
                new ZLinkAuthorityStored(
                    stored.storeVersion(),
                    stored.payload(),
                    stored.objectGeneration(),
                    stored.authorityOwnerGeneration(),
                    stored.ownerId(),
                    stored.ownerLeaseGeneration(),
                    stored.allocation(),
                    stored.storeNow()));
        }
    }

    private static final class FakeStagingBackend
        implements ZLinkUserSpotAggregateStagingOwner.StagingBackend {
        private final ArrayList<String> operations =
            new ArrayList<>();
        private final ArrayList<String> live =
            new ArrayList<>();
        private int replaySequence;

        @Override public CompletionStage<Object> prepareSpot(
            ZLinkUserSpotAggregateStagingOwner.Request request) {
            operations.add("prepare");
            return CompletableFuture.completedFuture("spot");
        }

        @Override public CompletionStage<Void> restoreSpot(
            Object prepared,
            ZLinkUserSpotAggregateStagingOwner.Request request,
            ZLinkRelocationCancellation cancellation) {
            operations.add("restore");
            return CompletableFuture.completedFuture(null);
        }

        @Override public CompletionStage<Object> prepareActor(
            ZLinkUserSpotAggregateStagingOwner.ActorParticipant participant,
            ZLinkRelocationCancellation cancellation) {
            operations.add("prepare-actor");
            return CompletableFuture.completedFuture(participant.actorId());
        }

        @Override public void publishSpot(Object prepared) {
            operations.add("publish");
            live.add((String) prepared);
        }

        @Override public void publishActor(Object prepared) {
            operations.add("publish-actor");
            live.add((String) prepared);
        }
        @Override public void completeActor(Object prepared) {
            operations.add("complete-actor");
        }
        @Override public void publishTimers(Object prepared) {
            operations.add("timers");
        }
        @Override public CompletionStage<List<byte[]>> replaySpot(
            Object prepared,
            ZLinkSpotAcceptedJournal.Record record) {
            int sequence = ++replaySequence;
            if (record.requestSequence().isPresent()) {
                operations.add("replay-request:" + sequence);
                return CompletableFuture.completedFuture(
                    List.of(new byte[] {9}));
            }
            operations.add("replay-send:" + sequence);
            return CompletableFuture.completedFuture(List.of());
        }
        @Override public CompletionStage<Void> discardActor(Object prepared) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public void discardSpot(Object prepared) { }
    }

    private static final class RestartActorBackend
        implements ZLinkStandaloneActorRelocationStagingOwner.Backend {
        private final List<String> operations;

        private RestartActorBackend(List<String> operations) {
            this.operations = operations;
        }

        @Override
        public CompletionStage<Object> prepare(
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            operations.add("prepare-actor");
            return CompletableFuture.completedFuture("actor");
        }

        @Override
        public CompletionStage<Optional<byte[]>> replay(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            ZLinkActorAcceptedJournal.Record record) {
            return CompletableFuture.failedFuture(new AssertionError(
                "the direct-Join restart root has no accepted journal"));
        }

        @Override
        public void publish(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            long targetOwnerGeneration) {
            operations.add("publish-actor");
        }

        @Override
        public void openAdmission(Object actor) {
            operations.add("open");
        }

        @Override
        public CompletionStage<Void> discard(Object actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override public ZLinkSpotContext context() { return null; }
        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
