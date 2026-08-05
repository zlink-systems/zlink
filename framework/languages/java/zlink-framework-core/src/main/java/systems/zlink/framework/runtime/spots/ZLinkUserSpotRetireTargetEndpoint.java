package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.Map;
import java.util.Objects;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationPermitPool;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkServiceRelocationEnvelopeCodec;
import systems.zlink.framework.spots.ZLinkSpot;

/**
 * Target-side owner for relocation stage, authority-gated publication and
 * pre-commit discard. A staged aggregate stays outside live registries until
 * the Location Store exposes the exact root and target-owner fence.
 */
final class ZLinkUserSpotRetireTargetEndpoint
    implements ZLinkSpotRetireControl.TargetEndpoint {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final ZLinkRelocationCancellation NOT_CANCELLED =
        () -> false;

    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkUserSpotAggregateStagingOwner staging;
    private final Function<String, Class<? extends ZLinkSpot<?>>> spotTypes;
    private final ZLinkUserSpotAggregateStagingOwner.JournalReplayer
        fallbackReplayer;
    private final ZLinkSessionRelocationPeerClient sessionRoutes;
    private final Duration routeTimeout;
    private final SteadyNormalizer normalizer;
    private final ZLinkSpotRuntime sourceReplies;
    private final ZLinkSpotRetireControl.Client relocationClient;
    private final ZLinkLocationRepository locations;
    private final ZLinkStandaloneActorRelocationStagingOwner actorStaging;
    private final ZLinkRelocationPermitPool permits;
    private final ZLinkServiceRelocationWireCodec relocationWire =
        new ZLinkServiceRelocationWireCodec();
    private final ConcurrentHashMap<ZLinkSpotRetireControl.Fence, TargetStage>
        stages = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<
        ZLinkSpotRetireControl.Fence, ActorTargetStage> actorStages =
            new ConcurrentHashMap<>();

    ZLinkUserSpotRetireTargetEndpoint(
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkUserSpotAggregateStagingOwner staging,
        Function<String, Class<? extends ZLinkSpot<?>>> spotTypes,
        ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer,
        ZLinkSessionRelocationPeerClient sessionRoutes,
        Duration routeTimeout,
        SteadyNormalizer normalizer) {
        this(
            localNodeRid,
            localNodeGeneration,
            coordinator,
            staging,
            spotTypes,
            replayer,
            sessionRoutes,
            routeTimeout,
            normalizer,
            null,
            null,
            null,
            null,
            null);
    }

    ZLinkUserSpotRetireTargetEndpoint(
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkUserSpotAggregateStagingOwner staging,
        Function<String, Class<? extends ZLinkSpot<?>>> spotTypes,
        ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer,
        ZLinkSessionRelocationPeerClient sessionRoutes,
        Duration routeTimeout,
        SteadyNormalizer normalizer,
        ZLinkSpotRuntime sourceReplies,
        ZLinkSpotRetireControl.Client relocationClient,
        ZLinkLocationRepository locations,
        ZLinkStandaloneActorRelocationStagingOwner actorStaging) {
        this(
            localNodeRid,
            localNodeGeneration,
            coordinator,
            staging,
            spotTypes,
            replayer,
            sessionRoutes,
            routeTimeout,
            normalizer,
            sourceReplies,
            relocationClient,
            locations,
            actorStaging,
            null);
    }

    ZLinkUserSpotRetireTargetEndpoint(
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkUserSpotAggregateStagingOwner staging,
        Function<String, Class<? extends ZLinkSpot<?>>> spotTypes,
        ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer,
        ZLinkSessionRelocationPeerClient sessionRoutes,
        Duration routeTimeout,
        SteadyNormalizer normalizer,
        ZLinkSpotRuntime sourceReplies,
        ZLinkSpotRetireControl.Client relocationClient,
        ZLinkLocationRepository locations,
        ZLinkStandaloneActorRelocationStagingOwner actorStaging,
        ZLinkRelocationPermitPool permits) {
        this.localNodeRid = Objects.requireNonNull(
            localNodeRid, "localNodeRid");
        if (localNodeGeneration <= 0) {
            throw new IllegalArgumentException(
                "local node generation must be positive");
        }
        this.localNodeGeneration = localNodeGeneration;
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.staging = Objects.requireNonNull(staging, "staging");
        this.spotTypes = Objects.requireNonNull(spotTypes, "spotTypes");
        fallbackReplayer = relocationClient == null
            ? Objects.requireNonNull(replayer, "replayer")
            : replayer;
        this.sessionRoutes = sessionRoutes;
        this.routeTimeout = Objects.requireNonNull(routeTimeout, "routeTimeout");
        this.normalizer = Objects.requireNonNull(normalizer, "normalizer");
        this.sourceReplies = sourceReplies;
        this.relocationClient = relocationClient;
        this.locations = locations;
        this.actorStaging = actorStaging;
        this.permits = permits;
    }

    @Override
    public CompletionStage<ZLinkSpotRelocationReplyRoutes.Ack> relayReply(
        RoutingId transportSource,
        ZLinkSpotRelocationReplyRoutes.Relay relay) {
        if (sourceReplies == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "relocation reply runtime is unavailable"));
        }
        try {
            return sourceReplies.relayRelocationReply(
                relay, transportSource);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    @Override
    public CompletionStage<Void> stage(
        ZLinkSpotRetireControl.StageRequest request) {
        validateTarget(request);
        if (isStandaloneActor(request)) {
            return stageActor(request);
        }
        return coordinator.readRoot(
                request.relocationReference(),
                request.relocationChecksum(),
                OPEN)
            .thenCompose(root -> {
                var decoded = ZLinkCanonicalUserSpotRelocationEnvelope.decode(
                    root.payload(),
                    localNodeRid,
                    spotTypes,
                    request);
                if (!decoded.spotId().equals(request.spotId())
                    || !decoded.spotStableType().equals(request.stableType())) {
                    return CompletableFuture.failedFuture(
                        new IllegalArgumentException(
                            "relocation root does not match the target Spot"));
                }
                return acquireInbound(request, root.payload().length)
                    .thenCompose(permit -> stageAggregate(
                        request,
                        decoded,
                        root.inventoryDigest(),
                        permit));
            });
    }

    @Override
    public CompletionStage<Void> publish(
        ZLinkSpotRetireControl.StageRequest request) {
        if (isStandaloneActor(request)) {
            return publishActor(request);
        }
        TargetStage target = requireStage(request);
        List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant>
            participants = request.participants().stream()
                .map(value -> new ZLinkAggregateRelocationCoordinator
                    .ExpectedParticipant(
                        value.authorityKey(),
                        value.objectGeneration(),
                        value.sourceAuthorityOwnerGeneration()))
                .toList();
        return coordinator.readPublishedAggregate(
                participants,
                new ZLinkAggregateFence(
                    request.fence().aggregateId(),
                    request.fence().aggregateGeneration()),
                new ZLinkLocationOwnerToken(
                    request.targetOwnerId(),
                    request.targetOwnerLeaseGeneration()),
                target.inventoryDigest(),
                OPEN)
            .thenCompose(root -> {
                var finalRequest = ZLinkCanonicalUserSpotRelocationEnvelope.decode(
                    root.payload(),
                    localNodeRid,
                    spotTypes,
                    request);
                if (!finalRequest.spotId().equals(request.spotId())
                    || !finalRequest.spotStableType().equals(
                        request.stableType())) {
                    return CompletableFuture.failedFuture(
                        new IllegalArgumentException(
                            "published relocation root does not match the target Spot"));
                }
                return staging.publishAndReplayHidden(
                    target.staged(),
                    finalRequest,
                    productionReplayer(target, request))
                    .thenRun(() -> {
                        target.published().set(true);
                    });
            });
    }

    private ZLinkUserSpotAggregateStagingOwner.JournalReplayer
        productionReplayer(
            TargetStage target,
            ZLinkSpotRetireControl.StageRequest request) {
        if (relocationClient == null) {
            return fallbackReplayer;
        }
        return new ZLinkAcceptedJournalReplayer(
            record -> staging.replaySpot(target.staged(), record),
            record -> staging.replayActor(target.staged(), record),
            new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                @Override
                public CompletionStage<Void> completeSpot(
                    ZLinkSpotAcceptedJournal.Record record,
                    long acceptedSequence,
                    List<byte[]> reply) {
                    RoutingId sourceNode = record.routingId().orElseThrow(
                        () -> new IllegalStateException(
                            "accepted request source node is unavailable"));
                    var participant = participant(
                        request,
                        request.spotId(),
                        record.requestSequence().orElse(0L));
                    ZLinkServiceRelocationEnvelopeCodec.Payload payload = null;
                    if (record.requestSequence().isPresent()) {
                        if (reply.size() != 1 || record.parts().isEmpty()) {
                            return CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "canonical Spot request must produce one reply"));
                        }
                        payload = new ZLinkServiceRelocationEnvelopeCodec.Payload(
                            new String(
                                record.parts().getFirst(),
                                java.nio.charset.StandardCharsets.UTF_8),
                            "application/zlink-framework-json-v1",
                            reply.getFirst());
                    }
                    return completeCanonicalReplay(
                        request,
                        participant,
                        acceptedSequence,
                        record.operationHigh(),
                        record.operationLow(),
                        record.requestSequence().orElse(0L),
                        record.sourceOwnerId(),
                        record.sourceOwnerLeaseGeneration(),
                        sourceNode,
                        record.sourceNodeGeneration(),
                        payload);
                }

                @Override
                public CompletionStage<Void> completeActor(
                    ZLinkActorAcceptedJournal.Record record,
                    long acceptedSequence,
                    java.util.Optional<byte[]> reply) {
                    ZLinkServiceRelocationEnvelopeCodec.Payload payload =
                        reply.map(bytes ->
                            new ZLinkServiceRelocationEnvelopeCodec.Payload(
                                record.header().name(),
                                actorContentType(record.header().codec()),
                                bytes))
                            .orElse(null);
                    return completeCanonicalReplay(
                        request,
                        participant(
                            request,
                            record.actorId(),
                            record.replyRouteId().orElse(0L)),
                        acceptedSequence,
                        record.operationHigh(),
                        record.operationLow(),
                        record.replyRouteId().orElse(0L),
                        record.sourceOwnerId(),
                        record.sourceOwnerLeaseGeneration(),
                        record.sourceNodeRid(),
                        record.sourceNodeGeneration(),
                        payload);
                }
            });
    }

    private CompletionStage<Void> completeCanonicalReplay(
        ZLinkSpotRetireControl.StageRequest request,
        ParticipantSlot participant,
        long acceptedSequence,
        long operationHigh,
        long operationLow,
        long replyRouteId,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceRelocationEnvelopeCodec.Payload payload) {
        if (replyRouteId != 0 && payload == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "canonical request replay produced no terminal payload"));
        }
        ZLinkServiceRelocationEnvelopeCodec.Completion completion =
            replyRouteId == 0 ? null
                : new ZLinkServiceRelocationEnvelopeCodec.Completion(
                    operationHigh,
                    operationLow,
                    sourceOwnerId,
                    sourceOwnerLeaseGeneration,
                    sourceNodeRid.toString(),
                    sourceNodeGeneration,
                    participant.id(),
                    acceptedSequence,
                    0,
                    0,
                    0,
                    payload);
        var owner = new ZLinkLocationOwnerToken(
            request.targetOwnerId(), request.targetOwnerLeaseGeneration());
        List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant>
            expected = expectedParticipants(request);
        return coordinator.updateCanonicalReplay(
                expected,
                new ZLinkAggregateFence(
                    request.fence().aggregateId(),
                    request.fence().aggregateGeneration()),
                owner,
                current -> ZLinkServiceRelocationEnvelopeCodec.advanceReplay(
                    current,
                    participant.id(),
                    acceptedSequence,
                    completion),
                OPEN)
            .thenCompose(progress -> completion == null
                ? CompletableFuture.completedFuture(null)
                : relayCanonicalWithRetry(
                    request,
                    participant,
                    completion,
                    progress,
                    1)
                    .thenCompose(ack -> completeCanonicalDelivery(
                        request,
                        participant,
                        expected,
                        owner,
                        completion,
                        ack)));
    }

    private CompletionStage<ZLinkServiceRelocationWireCodec.ReplyRelayAck>
        relayCanonicalWithRetry(
            ZLinkSpotRetireControl.StageRequest request,
            ParticipantSlot participant,
            ZLinkServiceRelocationEnvelopeCodec.Completion completion,
            ZLinkAggregateRelocationCoordinator.CanonicalProgress progress,
            int attempt) {
        var coordinatorFence = new ZLinkServiceRelocationWireCodec
            .CoordinatorFence(
                request.targetOwnerId(),
                request.targetOwnerLeaseGeneration(),
                localNodeRid,
                localNodeGeneration,
                progress.storeVersion(participant.id()));
        var relay = new ZLinkServiceRelocationWireCodec.ReplyRelay(
            new ZLinkServiceRelocationWireCodec.Operation(
                completion.operationHigh(), completion.operationLow()),
            participant.replyRouteId(),
            new ZLinkServiceRelocationWireCodec.RelocationId(
                request.fence().aggregateId().getMostSignificantBits(),
                request.fence().aggregateId().getLeastSignificantBits()),
            progress.ownerGeneration(participant.id()),
            coordinatorFence,
            participant.id(),
            completion.sequence(),
            completion.terminalResult(),
            completion.failureCode());
        byte[] command = relocationWire.encodeReplyRelay(relay);
        var expectedSource = new ZLinkServiceRelocationWireCodec
            .RequestSourceFence(
                completion.sourceOwnerId(),
                completion.sourceOwnerLeaseGeneration(),
                RoutingId.from(completion.sourceNodeRid()),
                completion.sourceNodeGeneration());
        return relocationClient.relayCanonicalReply(
                RoutingId.from(completion.sourceNodeRid()),
                expectedSource,
                command,
                completion.payload() == null
                    ? List.of()
                    : List.of(completion.payload().bytes()),
                routeTimeout)
            .thenApply(encoded -> {
                var ack = relocationWire.decodeReplyRelayAck(encoded);
                validateCanonicalAck(relay, completion, ack);
                return ack;
            })
            .handle((ack, failure) -> {
                return new CanonicalRelayAttempt(ack, failure);
            })
            .thenCompose(result -> {
                if (result.failure() == null) {
                    return CompletableFuture.completedFuture(result.ack());
                }
                if (attempt >= 3) {
                    return CompletableFuture.completedFuture(null);
                }
                return CompletableFuture.supplyAsync(
                        () -> null,
                        CompletableFuture.delayedExecutor(
                            25L * attempt,
                            java.util.concurrent.TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> relayCanonicalWithRetry(
                        request,
                        participant,
                        completion,
                        progress,
                        attempt + 1));
            });
    }

    private CompletionStage<Void> completeCanonicalDelivery(
        ZLinkSpotRetireControl.StageRequest request,
        ParticipantSlot participant,
        List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant>
            participants,
        ZLinkLocationOwnerToken owner,
        ZLinkServiceRelocationEnvelopeCodec.Completion completion,
        ZLinkServiceRelocationWireCodec.ReplyRelayAck ack) {
        if (ack != null) {
            int deliveryState = ack.status() == 2 ? 2 : 1;
            CompletionStage<
                ZLinkAggregateRelocationCoordinator.CanonicalProgress>
                recorded = recordCanonicalDelivery(
                    participants, owner, completion, deliveryState);
            if (deliveryState == 2) {
                return recorded.thenApply(ignored -> null);
            }
            return recorded
                .thenCompose(progress -> relayCanonicalWithRetry(
                    request,
                    participant,
                    completion,
                    progress,
                    1))
                .thenCompose(confirmed -> {
                    if (confirmed == null) {
                        return completeCanonicalDelivery(
                            request,
                            participant,
                            participants,
                            owner,
                            completion,
                            null);
                    }
                    if (confirmed.status() != 2) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "duplicate reply relay did not return "
                                    + "AlreadyTerminal"));
                    }
                    return recordCanonicalDelivery(
                            participants, owner, completion, 2)
                        .thenApply(ignored -> null);
                });
        }
        if (locations == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "source lease store is unavailable after reply ACK loss"));
        }
        return locations.readOwnerLease(completion.sourceOwnerId())
            .thenCompose(lease -> {
                boolean expired = lease instanceof ZLinkOwnerLeaseMissing
                    || lease instanceof ZLinkOwnerLeaseFound found
                        && (!found.token().ownerId().equals(
                                completion.sourceOwnerId())
                            || found.token().leaseGeneration()
                                != completion.sourceOwnerLeaseGeneration()
                            || !found.leaseExpiresAt().isAfter(
                                found.storeNow()));
                if (!expired) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "reply has no ACK and exact source lease is active"));
                }
                return coordinator.updateCanonicalReplay(
                        participants,
                        new ZLinkAggregateFence(
                            request.fence().aggregateId(),
                            request.fence().aggregateGeneration()),
                        owner,
                        current -> ZLinkServiceRelocationEnvelopeCodec
                            .completeDelivery(
                                current,
                                completion.operationHigh(),
                                completion.operationLow(),
                                completion.sourceOwnerId(),
                                completion.sourceOwnerLeaseGeneration(),
                                RoutingId.from(completion.sourceNodeRid()),
                                completion.sourceNodeGeneration(),
                                3),
                        OPEN)
                    .thenApply(ignored -> null);
            });
    }

    private CompletionStage<
        ZLinkAggregateRelocationCoordinator.CanonicalProgress>
        recordCanonicalDelivery(
            List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant>
                participants,
            ZLinkLocationOwnerToken owner,
            ZLinkServiceRelocationEnvelopeCodec.Completion completion,
            int deliveryState) {
        return coordinator.updateCanonicalReplay(
            participants,
            owner,
            current -> ZLinkServiceRelocationEnvelopeCodec.completeDelivery(
                current,
                completion.operationHigh(),
                completion.operationLow(),
                completion.sourceOwnerId(),
                completion.sourceOwnerLeaseGeneration(),
                RoutingId.from(completion.sourceNodeRid()),
                completion.sourceNodeGeneration(),
                deliveryState),
            OPEN);
    }

    CompletionStage<byte[]> relayCanonicalReply(
        RoutingId transportSource,
        byte[] command33,
        List<byte[]> payload) {
        ZLinkServiceRelocationWireCodec.ReplyRelay relay =
            relocationWire.decodeReplyRelay(command33);
        if (sourceReplies == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "relocation reply runtime is unavailable"));
        }
        var route = sourceReplies.canonicalRelocationReplyRoute(
            relay, transportSource);
        if (route == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "canonical reply capability fence differs"));
        }
        return coordinator.readCanonicalReply(
                route.authorityKey(),
                route.objectGeneration(),
                route.sourceOwnerId(),
                route.sourceOwnerLeaseGeneration(),
                route.sourceNodeRid(),
                route.sourceNodeGeneration(),
                relay,
                OPEN)
            .thenCompose(completion -> {
                List<byte[]> canonicalPayload = completion.payload() == null
                    ? List.of()
                    : List.of(completion.payload().bytes());
                if (!sameParts(payload, canonicalPayload)) {
                    return CompletableFuture.failedFuture(
                        new IllegalArgumentException(
                            "command 33 payload differs from durable completion"));
                }
                return sourceReplies.deliverCanonicalRelocationReply(
                    route, canonicalPayload);
            })
            .thenApply(ack -> {
                if (ack == ZLinkSpotRelocationReplyRoutes.Ack
                    .NOT_ACKNOWLEDGED) {
                    throw new IllegalStateException(
                        "canonical reply capability is busy");
                }
                int status = ack == ZLinkSpotRelocationReplyRoutes.Ack
                    .ALREADY_TERMINAL ? 2 : 1;
                return relocationWire.encodeReplyRelayAck(
                    new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                        relay.relocation(),
                        relay.coordinator(),
                        relay.operation(),
                        relay.replyRouteId(),
                        new ZLinkServiceRelocationWireCodec
                            .RequestSourceFence(
                                route.sourceOwnerId(),
                                route.sourceOwnerLeaseGeneration(),
                                route.sourceNodeRid(),
                                route.sourceNodeGeneration()),
                        status));
            });
    }

    static void validateCanonicalAck(
        ZLinkServiceRelocationWireCodec.ReplyRelay relay,
        ZLinkServiceRelocationEnvelopeCodec.Completion completion,
        ZLinkServiceRelocationWireCodec.ReplyRelayAck ack) {
        if (!ack.relocation().equals(relay.relocation())
            || !ack.coordinator().equals(relay.coordinator())
            || !ack.operation().equals(relay.operation())
            || ack.replyRouteId() != relay.replyRouteId()
            || !ack.requestSource().ownerId().equals(
                completion.sourceOwnerId())
            || ack.requestSource().leaseGeneration()
                != completion.sourceOwnerLeaseGeneration()
            || !ack.requestSource().nodeRid().equals(
                RoutingId.from(completion.sourceNodeRid()))
            || ack.requestSource().nodeGeneration()
                != completion.sourceNodeGeneration()) {
            throw new IllegalArgumentException(
                "command 46 fence differs from command 33");
        }
    }

    private static ParticipantSlot participant(
        ZLinkSpotRetireControl.StageRequest request,
        String objectId,
        long replyRouteId) {
        for (int index = 0; index < request.participants().size(); index++) {
            var candidate = request.participants().get(index);
            if (candidate.objectId().equals(objectId)) {
                return new ParticipantSlot(
                    index + 1L, candidate, replyRouteId);
            }
        }
        throw new IllegalStateException(
            "relocation participant is absent: " + objectId);
    }

    private static List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant>
        expectedParticipants(ZLinkSpotRetireControl.StageRequest request) {
        return request.participants().stream()
            .map(value -> new ZLinkAggregateRelocationCoordinator
                .ExpectedParticipant(
                    value.authorityKey(),
                    value.objectGeneration(),
                    value.sourceAuthorityOwnerGeneration()))
            .toList();
    }

    private static String actorContentType(
        systems.zlink.framework.streams.ZLinkStreamCodec codec) {
        return switch (codec) {
            case RAW -> "application/octet-stream";
            case JSON -> "application/json";
            case MESSAGE_PACK -> "application/msgpack";
            case PROTOBUF -> "application/protobuf";
        };
    }

    private static boolean sameParts(
        List<byte[]> left, List<byte[]> right) {
        if (left.size() != right.size()) return false;
        for (int index = 0; index < left.size(); index++) {
            if (!java.util.Arrays.equals(left.get(index), right.get(index))) {
                return false;
            }
        }
        return true;
    }

    private record CanonicalRelayAttempt(
        ZLinkServiceRelocationWireCodec.ReplyRelayAck ack,
        Throwable failure) {
    }

    private record ParticipantSlot(
        long id,
        ZLinkSpotRetireControl.ParticipantFence fence,
        long replyRouteId) {
    }

    @Override
    public CompletionStage<Void> finalizeAfterCompletion(
        ZLinkSpotRetireControl.StageRequest request) {
        if (isStandaloneActor(request)) {
            return finalizeActor(request);
        }
        TargetStage target = requireStage(request);
        if (!target.published().get()) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "relocation target is not published"));
        }
        List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant>
            participants = request.participants().stream()
                .map(value -> new ZLinkAggregateRelocationCoordinator
                    .ExpectedParticipant(
                        value.authorityKey(),
                        value.objectGeneration(),
                        value.sourceAuthorityOwnerGeneration()))
                .toList();
        return coordinator.verifyCompletedAggregate(
                participants,
                new ZLinkAggregateFence(
                    request.fence().aggregateId(),
                    request.fence().aggregateGeneration()),
                new ZLinkLocationOwnerToken(
                    request.targetOwnerId(),
                    request.targetOwnerLeaseGeneration()),
                OPEN)
            .thenCompose(ignored -> switchSessionRoutes(request))
            .thenCompose(ignored -> normalizer.normalize(request))
            .thenRun(() -> {
                staging.openAdmission(target.staged());
                target.finalized().set(true);
                if (!stages.remove(request.fence(), target)) {
                    throw new IllegalStateException(
                        "relocation recovery pointer was already released");
                }
                close(target.permit());
            });
    }

    private CompletionStage<Void> switchSessionRoutes(
        ZLinkSpotRetireControl.StageRequest request) {
        if (request.sessionRoutes().isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        if (sessionRoutes == null || routeTimeout.isZero()
            || routeTimeout.isNegative()) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "bound Session route switch runtime is unavailable"));
        }
        return coordinator.readTargetOwnerGenerations(
                expectedParticipants(request),
                new ZLinkAggregateFence(
                    request.fence().aggregateId(),
                    request.fence().aggregateGeneration()),
                new ZLinkLocationOwnerToken(
                    request.targetOwnerId(),
                    request.targetOwnerLeaseGeneration()),
                OPEN)
            .thenCompose(generations -> {
                CompletionStage<Void> chain =
                    CompletableFuture.completedFuture(null);
                for (ZLinkSpotRetireControl.SessionRouteFence route
                    : request.sessionRoutes()) {
                    String authorityKey = request.participants().stream()
                        .filter(participant -> participant.objectId().equals(
                            route.actorId()))
                        .map(ZLinkSpotRetireControl.ParticipantFence
                            ::authorityKey)
                        .findFirst()
                        .orElseThrow(() -> new IllegalStateException(
                            "session route participant is absent: "
                                + route.actorId()));
                    long targetOwnerGeneration = generations.getOrDefault(
                        authorityKey,
                        0L);
                    var command = routeCommand(
                        request,
                        route,
                        targetOwnerGeneration);
                    chain = chain.thenCompose(ignored ->
                        sessionRoutes.switchRoute(command, routeTimeout)
                            .thenApply(ack -> null));
                }
                return chain;
            });
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute
        routeCommand(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkSpotRetireControl.SessionRouteFence route,
            long targetOwnerGeneration) {
        if (targetOwnerGeneration <= 0) {
            throw new IllegalArgumentException(
                "session route target owner generation is invalid");
        }
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(
                request.fence().aggregateId().getMostSignificantBits(),
                request.fence().aggregateId().getLeastSignificantBits()),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                request.sourceOwnerId(),
                request.sourceOwnerLeaseGeneration(),
                request.sourceNodeRid(),
                request.sourceNodeGeneration(),
                route.sourceAuthorityStoreVersion()),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity(
                route.actorId(), route.actorObjectGeneration()),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                route.sessionOwnerNodeRid(),
                route.sessionOwnerNodeGeneration(),
                route.sessionOwnerId(),
                route.sessionOwnerLeaseGeneration(),
                route.sessionRid(),
                route.bindingGeneration()),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            route.sourceAuthorityOwnerGeneration(),
            targetOwnerGeneration,
            request.targetNodeRid(),
            request.targetNodeGeneration(),
            route.lastAcceptedSessionSequence());
    }

    @Override
    public CompletionStage<Void> abort(
        ZLinkSpotRetireControl.StageRequest request) {
        if (isStandaloneActor(request)) {
            return abortActor(request);
        }
        TargetStage target = requireStage(request);
        return staging.discard(target.staged())
            .thenRun(() -> {
                if (stages.remove(request.fence(), target)) {
                    close(target.permit());
                }
            });
    }

    private void validateTarget(ZLinkSpotRetireControl.StageRequest request) {
        if (!request.targetNodeRid().equals(localNodeRid)
            || request.targetNodeGeneration() != localNodeGeneration) {
            throw new IllegalArgumentException(
                "relocation target node fence is stale");
        }
    }

    private CompletionStage<Void> stageActor(
        ZLinkSpotRetireControl.StageRequest request) {
        if (actorStaging == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "standalone Actor relocation target is unavailable"));
        }
        var participant = request.participants().getFirst();
        var targetRequest =
            new ZLinkStandaloneActorRelocationStagingOwner.Request(
                request.fence().aggregateId(),
                participant.objectId(),
                participant.stableType(),
                participant.objectGeneration(),
                participant.sourceAuthorityOwnerGeneration(),
                participant.restoreSnapshot(),
                request.spotId());
        return coordinator.readRoot(
                request.relocationReference(),
                request.relocationChecksum(),
                OPEN)
            .thenCompose(root -> acquireInbound(request, root.payload().length)
                .thenCompose(permit -> stageStandaloneActor(
                    request,
                    targetRequest,
                    root.payload(),
                    root.inventoryDigest(),
                    permit)));
    }

    private CompletionStage<Void> publishActor(
        ZLinkSpotRetireControl.StageRequest request) {
        ActorTargetStage target = requireActorStage(request);
        var participant = request.participants().getFirst();
        return coordinator.readPublishedAggregate(
                expectedParticipants(request),
                new ZLinkAggregateFence(
                    request.fence().aggregateId(),
                    request.fence().aggregateGeneration()),
                new ZLinkLocationOwnerToken(
                    request.targetOwnerId(),
                    request.targetOwnerLeaseGeneration()),
                coordinatorInventoryDigest(request),
                OPEN)
            .thenCompose(root -> actorStaging.publishAndReplayHidden(
                target.staged(),
                root.payload(),
                root.targetOwnerGeneration(participant.authorityKey()),
                productionActorReplayer(target, request)))
            .thenRun(() -> target.published().set(true));
    }

    private ZLinkUserSpotAggregateStagingOwner.JournalReplayer
        productionActorReplayer(
            ActorTargetStage target,
            ZLinkSpotRetireControl.StageRequest request) {
        if (relocationClient == null) {
            return fallbackReplayer;
        }
        return new ZLinkAcceptedJournalReplayer(
            record -> CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "standalone Actor root contains a Spot journal")),
            record -> actorStaging.replayActor(target.staged(), record),
            new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                @Override
                public CompletionStage<Void> completeSpot(
                    ZLinkSpotAcceptedJournal.Record record,
                    long acceptedSequence,
                    List<byte[]> reply) {
                    return CompletableFuture.failedFuture(
                        new IllegalArgumentException(
                            "standalone Actor root contains a Spot journal"));
                }

                @Override
                public CompletionStage<Void> completeActor(
                    ZLinkActorAcceptedJournal.Record record,
                    long acceptedSequence,
                    java.util.Optional<byte[]> reply) {
                    ZLinkServiceRelocationEnvelopeCodec.Payload payload =
                        reply.map(bytes ->
                            new ZLinkServiceRelocationEnvelopeCodec.Payload(
                                record.header().name(),
                                actorContentType(record.header().codec()),
                                bytes))
                            .orElse(null);
                    return completeCanonicalReplay(
                        request,
                        participant(
                            request,
                            record.actorId(),
                            record.replyRouteId().orElse(0L)),
                        acceptedSequence,
                        record.operationHigh(),
                        record.operationLow(),
                        record.replyRouteId().orElse(0L),
                        record.sourceOwnerId(),
                        record.sourceOwnerLeaseGeneration(),
                        record.sourceNodeRid(),
                        record.sourceNodeGeneration(),
                        payload);
                }
            });
    }

    private CompletionStage<Void> finalizeActor(
        ZLinkSpotRetireControl.StageRequest request) {
        ActorTargetStage target = requireActorStage(request);
        if (!target.published().get()) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "standalone Actor relocation target is not published"));
        }
        return coordinator.verifyCompletedAggregate(
                expectedParticipants(request),
                new ZLinkAggregateFence(
                    request.fence().aggregateId(),
                    request.fence().aggregateGeneration()),
                new ZLinkLocationOwnerToken(
                    request.targetOwnerId(),
                    request.targetOwnerLeaseGeneration()),
                OPEN)
            .thenCompose(ignored -> switchSessionRoutes(request))
            .thenCompose(ignored -> normalizer.normalize(request))
            .thenRun(() -> {
                actorStaging.openAdmission(target.staged());
                if (actorStages.remove(request.fence(), target)) {
                    close(target.permit());
                }
            });
    }

    private CompletionStage<Void> abortActor(
        ZLinkSpotRetireControl.StageRequest request) {
        ActorTargetStage target = requireActorStage(request);
        return actorStaging.discard(target.staged())
            .thenRun(() -> {
                if (actorStages.remove(request.fence(), target)) {
                    close(target.permit());
                }
            });
    }

    private ActorTargetStage requireActorStage(
        ZLinkSpotRetireControl.StageRequest request) {
        validateTarget(request);
        ActorTargetStage target = actorStages.get(request.fence());
        if (target == null || !target.request().equals(request)) {
            throw new IllegalStateException(
                "standalone Actor target stage is unavailable");
        }
        return target;
    }

    private byte[] coordinatorInventoryDigest(
        ZLinkSpotRetireControl.StageRequest request) {
        ActorTargetStage target = requireActorStage(request);
        return target.inventoryDigest();
    }

    private static boolean isStandaloneActor(
        ZLinkSpotRetireControl.StageRequest request) {
        return request.participants().size() == 1
            && request.participants().getFirst().objectKind() == 1;
    }

    private TargetStage requireStage(
        ZLinkSpotRetireControl.StageRequest request) {
        validateTarget(request);
        TargetStage target = stages.get(request.fence());
        if (target == null || !target.request().equals(request)) {
            throw new IllegalStateException(
                "relocation target stage is unavailable");
        }
        return target;
    }

    int activeRecoveryPointerCount() {
        return stages.size();
    }

    /**
     * Releases the target recovery pointer only after durable reply relay or
     * source-lease expiry proves that recovery no longer needs the old root.
     */
    void releaseRecoveryPointer(
        ZLinkSpotRetireControl.StageRequest request,
        RecoveryReleaseEvidence evidence) {
        Objects.requireNonNull(evidence, "evidence");
        TargetStage target = requireStage(request);
        if (!target.finalized().get()) {
            throw new IllegalStateException(
                "unfinished relocation cannot release its recovery pointer");
        }
        if (!stages.remove(request.fence(), target)) {
            throw new IllegalStateException(
                "relocation recovery pointer was already released");
        }
        close(target.permit());
    }

    private CompletionStage<ZLinkRelocationPermitPool.Lease> acquireInbound(
        ZLinkSpotRetireControl.StageRequest request,
        long payloadBytes) {
        if (permits == null) {
            return CompletableFuture.completedFuture(null);
        }
        boolean restore = request.restoreSpotSnapshot()
            || request.participants().stream()
                .anyMatch(ZLinkSpotRetireControl.ParticipantFence
                    ::restoreSnapshot);
        return permits.acquire(
            ZLinkRelocationPermitPool.Request.inbound(
                payloadBytes,
                restore,
                true),
            OPEN);
    }

    private CompletionStage<Void> stageAggregate(
        ZLinkSpotRetireControl.StageRequest request,
        ZLinkUserSpotAggregateStagingOwner.Request decoded,
        byte[] inventoryDigest,
        ZLinkRelocationPermitPool.Lease permit) {
        AtomicBoolean retained = new AtomicBoolean();
        return staging.stage(decoded, NOT_CANCELLED)
            .thenCompose(value -> {
                TargetStage target = new TargetStage(
                    request,
                    inventoryDigest,
                    value,
                    new AtomicBoolean(),
                    new AtomicBoolean(),
                    permit);
                if (stages.putIfAbsent(request.fence(), target) == null) {
                    retained.set(true);
                    return CompletableFuture.<Void>completedFuture(null);
                }
                return staging.discard(value).thenCompose(ignored ->
                    CompletableFuture.<Void>failedFuture(
                        new IllegalStateException(
                            "relocation target stage already exists")));
            })
            .whenComplete((ignored, failure) -> {
                if (!retained.get()) {
                    close(permit);
                }
            });
    }

    private CompletionStage<Void> stageStandaloneActor(
        ZLinkSpotRetireControl.StageRequest request,
        ZLinkStandaloneActorRelocationStagingOwner.Request targetRequest,
        byte[] payload,
        byte[] inventoryDigest,
        ZLinkRelocationPermitPool.Lease permit) {
        AtomicBoolean retained = new AtomicBoolean();
        return actorStaging.stage(targetRequest, payload)
            .thenCompose(staged -> {
                ActorTargetStage target = new ActorTargetStage(
                    request,
                    targetRequest,
                    staged,
                    new AtomicBoolean(),
                    inventoryDigest,
                    permit);
                if (actorStages.putIfAbsent(request.fence(), target) == null) {
                    retained.set(true);
                    return CompletableFuture.<Void>completedFuture(null);
                }
                return actorStaging.discard(staged).thenCompose(ignored ->
                    CompletableFuture.<Void>failedFuture(
                        new IllegalStateException(
                            "standalone Actor target stage already exists")));
            })
            .whenComplete((ignored, failure) -> {
                if (!retained.get()) {
                    close(permit);
                }
            });
    }

    private static void close(ZLinkRelocationPermitPool.Lease permit) {
        if (permit != null) {
            permit.close();
        }
    }

    enum RecoveryReleaseEvidence {
        REPLY_RELAY_ACK,
        SOURCE_LEASE_EXPIRED
    }

    @FunctionalInterface
    interface SteadyNormalizer {
        CompletionStage<Void> normalize(
            ZLinkSpotRetireControl.StageRequest request);
    }

    private record TargetStage(
        ZLinkSpotRetireControl.StageRequest request,
        byte[] inventoryDigest,
        ZLinkUserSpotAggregateStagingOwner.Staged staged,
        AtomicBoolean published,
        AtomicBoolean finalized,
        ZLinkRelocationPermitPool.Lease permit) {
        private TargetStage {
            inventoryDigest = Objects.requireNonNull(
                inventoryDigest,
                "inventoryDigest").clone();
            Objects.requireNonNull(published, "published");
            Objects.requireNonNull(finalized, "finalized");
        }

        @Override public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }
    }

    private record ActorTargetStage(
        ZLinkSpotRetireControl.StageRequest request,
        ZLinkStandaloneActorRelocationStagingOwner.Request targetRequest,
        ZLinkStandaloneActorRelocationStagingOwner.Staged staged,
        AtomicBoolean published,
        byte[] inventoryDigest,
        ZLinkRelocationPermitPool.Lease permit) {
        ActorTargetStage {
            inventoryDigest = inventoryDigest.clone();
        }

        @Override public byte[] inventoryDigest() {
            return inventoryDigest == null ? null : inventoryDigest.clone();
        }
    }
}
