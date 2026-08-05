package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
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
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
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

    @Test
    void targetStaysInvisibleUntilExactAuthorityRootIsPublished() {
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
        recoveredEndpoint.publish(request).toCompletableFuture().join();

        assertEquals(List.of(actorId, "spot"), recoveredBackend.live);
        assertEquals(List.of(
            "prepare", "restore", "prepare-actor", "replay-send:1",
            "replay-request:2",
            "publish-actor", "publish"),
            recoveredBackend.operations);
        assertEquals(2, relays.get(),
            "command 33 is retried until command 46 reports terminal");
        assertThrows(java.util.concurrent.CompletionException.class, () ->
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
            "publish-actor", "publish", "command44", "normalize",
            "complete-actor", "timers"),
            recoveredBackend.operations);
        assertEquals(0, recoveredEndpoint.activeRecoveryPointerCount(),
            "completed root with no pending relay releases the target slot");
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
                                command.currentAuthorityOwnerGeneration(),
                                command.lastAcceptedSessionSequence())));
                }
                if (method.getDeclaringClass() == Object.class) {
                    return method.invoke(proxy, arguments);
                }
                throw new UnsupportedOperationException(method.getName());
            });
    }

    private static ZLinkInternalMeshNode recoveryNode(
        ZLinkServiceM6BWireCodec wire,
        List<String> operations,
        AtomicInteger relays) {
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
                                    command
                                        .currentAuthorityOwnerGeneration(),
                                    command
                                        .lastAcceptedSessionSequence())));
                }
                if (method.getName().equals(
                    "requestRelocationReplyRelay")) {
                    var expected = (systems.zlink.framework.runtime.internal
                        .service.ZLinkServiceRelocationWireCodec
                            .RequestSourceFence) arguments[1];
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
            if (prepared == null
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
                        ZLinkPlacementObjectKind.USER_SPOT,
                        "room",
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
        private final java.util.ArrayList<String> operations =
            new java.util.ArrayList<>();
        private final java.util.ArrayList<String> live =
            new java.util.ArrayList<>();
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
