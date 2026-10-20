package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.diagnostics.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.streams.ZLinkStreamCodec;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.logging.Logger;

/**
 * Target-side owner for relocation stage, authority-gated publication and pre-commit discard. A
 * staged aggregate stays outside live registries until the Location Store exposes the exact root
 * and target-owner fence.
 */
final class ZLinkUserSpotRetireTargetEndpoint
        implements ZLinkSpotRetireControl.TargetEndpoint,
                ZLinkInternalSpotNode.RelocationStagingIngressHandler {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkUserSpotRetireTargetEndpoint.class.getName());
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final ZLinkRelocationCancellation NOT_CANCELLED = () -> false;

    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkUserSpotAggregateStagingOwner staging;
    private final Function<String, Class<? extends ZLinkSpot<?>>> spotTypes;
    private final ZLinkUserSpotAggregateStagingOwner.JournalReplayer fallbackReplayer;
    private final ZLinkSessionRelocationPeerClient sessionRoutes;
    private final Duration routeTimeout;
    private final SteadyNormalizer normalizer;
    private final BiFunction<
                    ZLinkSpotRetireControl.StageRequest, Map<String, Long>, CompletionStage<Void>>
            boundSessionRestorer;
    private final ZLinkSpotRuntime sourceReplies;
    private final ZLinkSpotRetireControl.Client relocationClient;
    private final ZLinkLocationRepository locations;
    private final ZLinkStandaloneActorRelocationStagingOwner actorStaging;
    private final ZLinkActorJoinCanonicalAdapter actorJoin;
    private final ZLinkServiceRelocationWireCodec relocationWire =
            new ZLinkServiceRelocationWireCodec();
    private final ZLinkServiceM6BWireCodec serviceWire = new ZLinkServiceM6BWireCodec();
    private final ConcurrentHashMap<ZLinkSpotRetireControl.Fence, TargetStage> stages =
            new ConcurrentHashMap<>();
    private final ConcurrentHashMap<ZLinkSpotRetireControl.Fence, ActorTargetStage> actorStages =
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
                null,
                (ignored, generations) -> CompletableFuture.completedFuture(null));
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
                null,
                (ignored, generations) -> CompletableFuture.completedFuture(null));
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
            ZLinkActorJoinCanonicalAdapter actorJoin,
            BiFunction<
                            ZLinkSpotRetireControl.StageRequest,
                            Map<String, Long>,
                            CompletionStage<Void>>
                    boundSessionRestorer) {
        this.localNodeRid = Objects.requireNonNull(localNodeRid, "localNodeRid");
        // localNodeGeneration is a node lifecycle-generation opaque equality
        // token (.NET ulong, spec 01-glossary "Lifecycle generation"): full
        // range, only zero is unassigned. A signed `<= 0` sentinel wrongly
        // rejects a legitimate negative-as-long value.
        if (localNodeGeneration == 0) {
            throw new IllegalArgumentException("local node generation must be nonzero");
        }
        this.localNodeGeneration = localNodeGeneration;
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.staging = Objects.requireNonNull(staging, "staging");
        this.spotTypes = Objects.requireNonNull(spotTypes, "spotTypes");
        fallbackReplayer =
                relocationClient == null ? Objects.requireNonNull(replayer, "replayer") : replayer;
        this.sessionRoutes = sessionRoutes;
        this.routeTimeout = Objects.requireNonNull(routeTimeout, "routeTimeout");
        this.normalizer = Objects.requireNonNull(normalizer, "normalizer");
        this.boundSessionRestorer =
                Objects.requireNonNull(boundSessionRestorer, "boundSessionRestorer");
        this.sourceReplies = sourceReplies;
        this.relocationClient = relocationClient;
        this.locations = locations;
        this.actorStaging = actorStaging;
        this.actorJoin = actorJoin;
    }

    @Override
    public CompletionStage<ZLinkSpotRelocationReplyRoutes.Ack> relayReply(
            RoutingId transportSource, ZLinkSpotRelocationReplyRoutes.Relay relay) {
        if (sourceReplies == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("relocation reply runtime is unavailable"));
        }
        try {
            return sourceReplies.relayRelocationReply(relay, transportSource);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    @Override
    public CompletionStage<Void> beginPrepare(
            UUID relocationId, CompletionStage<Void> terminal, Runnable superseded) {
        return actorJoin == null
                ? CompletableFuture.completedFuture(null)
                : actorJoin.beginTarget(relocationId, terminal, superseded);
    }

    @Override
    public CompletionStage<Void> endPrepare(UUID relocationId) {
        return actorJoin == null
                ? CompletableFuture.completedFuture(null)
                : actorJoin.endTarget(relocationId);
    }

    @Override
    public ZLinkSpotRetireControl.TargetProfile applyTargetProfile(
            ZLinkSpotRetireControl.StageRequest request, long defaultActorSpotGeneration) {
        if (actorJoin != null && isStandaloneActor(request)) {
            actorJoin.claimRecovery(request);
        }
        return actorJoin == null
                ? ZLinkSpotRetireControl.TargetEndpoint.super.applyTargetProfile(
                        request, defaultActorSpotGeneration)
                : actorJoin.applyTargetProfile(request, defaultActorSpotGeneration);
    }

    @Override
    public CompletionStage<Void> stage(ZLinkSpotRetireControl.StageRequest request) {
        validateTarget(request);
        if (isStandaloneActor(request)) {
            return stageActor(request);
        }
        //  The directly transferred payload arrives assembled and
        //  checksum-verified with the stage request (spec 28 §4.3).
        var decoded =
                ZLinkCanonicalUserSpotRelocationEnvelope.decode(
                        request.relocationPayload(), localNodeRid, spotTypes, request);
        if (!decoded.spotId().equals(request.spotId())
                || !decoded.spotStableType().equals(request.stableType())) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException(
                            "relocation payload does not match the target Spot"));
        }
        return stageAggregate(request, decoded);
    }

    @Override
    public CompletionStage<Void> publish(ZLinkSpotRetireControl.StageRequest request) {
        if (isStandaloneActor(request)) {
            return publishActor(request);
        }
        TargetStage target = requireStage(request);
        return coordinator
                .readTargetOwnerGenerations(
                        expectedParticipants(request),
                        new ZLinkAggregateFence(
                                request.fence().aggregateId(),
                                request.fence().aggregateGeneration()),
                        new ZLinkLocationOwnerToken(
                                request.targetOwnerId(), request.targetOwnerLeaseGeneration()),
                        OPEN)
                .thenCompose(
                        generations -> {
                            var finalRequest =
                                    ZLinkCanonicalUserSpotRelocationEnvelope.decode(
                                            request.relocationPayload(),
                                            localNodeRid,
                                            spotTypes,
                                            request);
                            if (!finalRequest.spotId().equals(request.spotId())
                                    || !finalRequest
                                            .spotStableType()
                                            .equals(request.stableType())) {
                                return CompletableFuture.failedFuture(
                                        new IllegalArgumentException(
                                                "published relocation payload does not match the"
                                                        + " target Spot"));
                            }
                            Map<String, Long> actorOwnerGenerations = new HashMap<>();
                            for (var participant : request.participants()) {
                                if (participant.objectKind()
                                        == ZLinkPlacementObjectKind.ACTOR.value()) {
                                    actorOwnerGenerations.put(
                                            participant.objectId(),
                                            requireGeneration(
                                                    generations, participant.authorityKey()));
                                }
                            }
                            return staging.closeDurableBacklog(
                                            target.staged(),
                                            finalRequest,
                                            productionReplayer(target, request))
                                    .thenCompose(
                                            backlog -> {
                                                return staging.publishHidden(
                                                                backlog, actorOwnerGenerations)
                                                        .thenCompose(
                                                                ignored ->
                                                                        boundSessionRestorer.apply(
                                                                                request,
                                                                                actorOwnerGenerations))
                                                        .thenCompose(
                                                                ignored ->
                                                                        staging.drainDurableBacklog(
                                                                                backlog));
                                            })
                                    .thenCompose(
                                            ignored -> switchSessionRoutes(request, generations))
                                    .thenCompose(ignored -> normalizer.normalize(request))
                                    .thenCompose(ignored -> staging.openAdmission(target.staged()))
                                    .thenRun(() -> releasePublishedTarget(request, target));
                        });
    }

    @Override
    public CompletionStage<Void> stageRelayedRecord(
            ZLinkSpotRetireControl.StageRequest request, byte[] frozenRecord) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(frozenRecord, "frozenRecord");
        if (isStandaloneActor(request)) {
            ActorTargetStage target = requireActorStage(request);
            return actorStaging.stageRelayedRecord(target.staged(), frozenRecord);
        }
        ZLinkActorAcceptedJournal.Record actorRecord;
        try {
            actorRecord = ZLinkActorAcceptedJournal.decode(frozenRecord);
        } catch (IllegalArgumentException notActorRecord) {
            actorRecord = null;
        }
        return staging.stageRelayedRecord(
                requireStage(request).staged(),
                actorRecord == null ? request.spotId() : actorRecord.actorId(),
                actorRecord != null,
                frozenRecord);
    }

    @Override
    public boolean handleSpot(
            ZLinkInternalMeshNode.PeerAuthorityFence source,
            ZLinkServiceM6BWireCodec.SpotMessage header,
            byte[] metadata,
            Supplier<byte[]> acceptedJournalRecord,
            int acceptedJournalRecordSizeHint,
            List<Message> parts,
            String contentType,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure) {
        TargetStage target =
                stages.values().stream()
                        .filter(
                                candidate ->
                                        matchesSource(candidate.request(), source)
                                                && matchesSpotIngress(
                                                        candidate.request(), header.target()))
                        .findFirst()
                        .orElse(null);
        if (target == null) {
            return false;
        }
        boolean accepted =
                staging.acceptSpotIngress(
                        target.staged(), acceptedJournalRecord.get(), reply, failure);
        if (accepted) {
            parts.forEach(Message::close);
        }
        return accepted;
    }

    @Override
    public boolean handleActor(
            ZLinkInternalMeshNode.PeerAuthorityFence source,
            ZLinkServiceM6BWireCodec.ActorMessage header,
            Supplier<byte[]> acceptedJournalRecord,
            List<Message> parts,
            String contentType,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure) {
        //  Only one relocation attempt can exist for a given standalone
        //  Actor at a time (spec 15 §4.2 "같은 object의 relocation
        //  temporary queue는 하나만 존재한다") — the registry evicts an
        //  older exact identity, live stage included, before a newer one
        //  is admitted or installed — so at most one candidate below can
        //  ever match.
        ActorTargetStage standalone =
                actorStages.values().stream()
                        .filter(
                                candidate ->
                                        matchesSource(candidate.request(), source)
                                                && matchesActorIngress(
                                                        candidate.request(), header.target()))
                        .findFirst()
                        .orElse(null);
        boolean accepted;
        if (standalone != null) {
            accepted =
                    actorStaging.acceptIngress(
                            standalone.staged(), acceptedJournalRecord.get(), reply, failure);
        } else if (actorJoin != null
                && tryRouteActorIngressToPrewarm(header, acceptedJournalRecord, reply, failure)) {
            //  No real stage installed yet, but a canonical Actor Join
            //  admission already parked this arrival in the relocation
            //  temporary queue (spec 15 §4.2) — PREPARE migrates it into
            //  the real staged queue once it installs.
            accepted = true;
        } else {
            TargetStage aggregate =
                    stages.values().stream()
                            .filter(
                                    candidate ->
                                            matchesSource(candidate.request(), source)
                                                    && matchesActorIngress(
                                                            candidate.request(), header.target()))
                            .findFirst()
                            .orElse(null);
            if (aggregate == null) {
                return false;
            }
            accepted =
                    staging.acceptActorIngress(
                            aggregate.staged(),
                            header.target().actor().actorId(),
                            acceptedJournalRecord.get(),
                            reply,
                            failure);
        }
        if (accepted) {
            parts.forEach(Message::close);
        }
        return accepted;
    }

    /**
     * Routes one Actor ingress arrival through the canonical Actor Join prewarm registry (spec 15
     * §4.2) when no real stage is installed in {@link #actorStages} yet. Delivers straight into the
     * real stage if PREPARE raced ahead and installed one between the caller's {@code actorStages}
     * lookup and this call, parks the arrival in the relocation temporary queue if only the
     * admission-time placeholder exists, or returns {@code false} if no attempt exists for the
     * object at all. Only ActorId + ObjectGeneration are known at admission time — the full
     * source/authority fence is not verified against a parked or racing-delivery arrival, matching
     * what the spec's prewarm phase itself has available.
     */
    private boolean tryRouteActorIngressToPrewarm(
            ZLinkServiceM6BWireCodec.ActorMessage header,
            Supplier<byte[]> acceptedJournalRecord,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure) {
        var actor = header.target().actor();
        var route =
                actorJoin.routeIngress(
                        actor.actorId(),
                        actor.generation(),
                        () ->
                                new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                                        acceptedJournalRecord.get(), reply, failure));
        return route != ZLinkActorJoinPrewarmRegistry.IngressRoute.NOT_FOUND;
    }

    private static boolean matchesSpotIngress(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkServiceM6BWireCodec.SpotRouteFence route) {
        var participant =
                request.participants().stream()
                        .filter(
                                value ->
                                        value.objectKind()
                                                == ZLinkPlacementObjectKind.USER_SPOT.value())
                        .findFirst()
                        .orElse(null);
        return participant != null
                && request.targetNodeRid().equals(route.targetNodeRid())
                && request.targetNodeGeneration() == route.targetNodeGeneration()
                && request.spotId().equals(route.spotId())
                && participant.objectGeneration() == route.spotGeneration()
                && participant.sourceAuthorityOwnerGeneration() == route.authorityOwnerGeneration()
                && request.targetOwnerLeaseGeneration() == route.ownerLeaseGeneration();
    }

    private static boolean matchesSource(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkInternalMeshNode.PeerAuthorityFence source) {
        return request.sourceNodeRid().equals(source.sourceNodeRid())
                && request.sourceNodeGeneration() == source.sourceNodeGeneration()
                && request.sourceOwnerId().equals(source.ownerId())
                && request.sourceOwnerLeaseGeneration() == source.ownerLeaseGeneration();
    }

    private static boolean matchesActorIngress(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkServiceM6BWireCodec.ActorRouteFence route) {
        var participant =
                request.participants().stream()
                        .filter(
                                value ->
                                        value.objectKind() == ZLinkPlacementObjectKind.ACTOR.value()
                                                && value.objectId().equals(route.actor().actorId()))
                        .findFirst()
                        .orElse(null);
        return participant != null
                && request.targetNodeRid().equals(route.actor().nodeRid())
                && request.targetNodeGeneration() == route.targetNodeGeneration()
                && participant.objectGeneration() == route.actor().generation()
                && participant.sourceAuthorityOwnerGeneration() == route.authorityOwnerGeneration()
                && request.targetOwnerLeaseGeneration() == route.ownerLeaseGeneration();
    }

    private ZLinkUserSpotAggregateStagingOwner.JournalReplayer productionReplayer(
            TargetStage target, ZLinkSpotRetireControl.StageRequest request) {
        if (relocationClient == null) {
            return fallbackReplayer;
        }
        return new ZLinkAcceptedJournalReplayer(
                record -> staging.replaySpot(target.staged(), record),
                record -> staging.replayActor(target.staged(), record),
                productionReplyRelay(request));
    }

    ZLinkAcceptedJournalReplayer.ReplyRelay productionReplyRelay(
            ZLinkSpotRetireControl.StageRequest request) {
        return new ZLinkAcceptedJournalReplayer.ReplyRelay() {
            @Override
            public CompletionStage<Void> completeSpot(
                    ZLinkSpotAcceptedJournal.Record record,
                    long acceptedSequence,
                    List<byte[]> reply,
                    Throwable failure) {
                RoutingId sourceNode =
                        record.routingId()
                                .orElseThrow(
                                        () ->
                                                new IllegalStateException(
                                                        "accepted request source node is"
                                                                + " unavailable"));
                var participant =
                        participant(request, request.spotId(), record.requestSequence().orElse(0L));
                CanonicalReplyPayload payload = null;
                if (failure == null && record.requestSequence().isPresent()) {
                    if (reply.size() != 1 || record.parts().isEmpty()) {
                        return CompletableFuture.failedFuture(
                                new IllegalStateException(
                                        "canonical Spot request must produce one reply"));
                    }
                    payload =
                            new CanonicalReplyPayload(
                                    new String(record.parts().getFirst(), StandardCharsets.UTF_8),
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
                        payload,
                        failure,
                        sourceReplies == null
                                        || !sourceReplies.dispatchErrorsEnabled()
                                        || record.parts().isEmpty()
                                ? null
                                : new String(record.parts().getFirst(), StandardCharsets.UTF_8));
            }

            @Override
            public CompletionStage<Void> completeActor(
                    ZLinkActorAcceptedJournal.Record record,
                    long acceptedSequence,
                    Optional<byte[]> reply,
                    Throwable failure) {
                CanonicalReplyPayload payload =
                        (reply == null ? Optional.<byte[]>empty() : reply)
                                .map(
                                        bytes ->
                                                new CanonicalReplyPayload(
                                                        record.header().name(),
                                                        actorContentType(record.header().codec()),
                                                        bytes))
                                .orElse(null);
                return completeCanonicalReplay(
                        request,
                        participant(request, record.actorId(), record.replyRouteId().orElse(0L)),
                        acceptedSequence,
                        record.operationHigh(),
                        record.operationLow(),
                        record.replyRouteId().orElse(0L),
                        record.sourceOwnerId(),
                        record.sourceOwnerLeaseGeneration(),
                        record.sourceNodeRid(),
                        record.sourceNodeGeneration(),
                        payload,
                        failure,
                        record.header().name());
            }
        };
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
            CanonicalReplyPayload payload,
            Throwable failure,
            String packetName) {
        if (replyRouteId != 0 && payload == null && failure == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "canonical request replay produced no terminal payload"));
        }
        boolean actor = participant.fence().objectKind() == ZLinkPlacementObjectKind.ACTOR.value();
        if (failure != null && sourceReplies != null && sourceReplies.dispatchErrorsEnabled()) {
            sourceReplies.reportDispatchError(
                    DispatchFailureReport.of(
                                    actor
                                            ? ZLinkDispatchErrorSurface.SPOT_ACTOR
                                            : ZLinkDispatchErrorSurface.SPOT_ROUTE,
                                    actor
                                            ? (replyRouteId == 0
                                                    ? ZLinkDispatchMessageKind.ACTOR_SEND
                                                    : ZLinkDispatchMessageKind.ACTOR_REQUEST)
                                            : (replyRouteId == 0
                                                    ? ZLinkDispatchMessageKind.SEND
                                                    : ZLinkDispatchMessageKind.REQUEST),
                                    ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                                    replyRouteId == 0
                                            ? ZLinkDispatchErrorAction.DROP
                                            : ZLinkDispatchErrorAction.REPLY_ERROR)
                            .packetName(packetName)
                            .spotId(request.spotId())
                            .actorId(actor ? participant.fence().objectId() : null)
                            .sourceRid(sourceNodeRid)
                            .correlationId(replyRouteId == 0 ? null : Long.toString(replyRouteId))
                            .error(failure));
        }
        if (replyRouteId == 0) {
            return CompletableFuture.completedFuture(null);
        }
        int[] terminal =
                failure == null
                        ? null
                        : ZLinkRequestFailureMapping.outgoing(
                                failure instanceof ZLinkFrameworkException framework
                                        ? framework.kind()
                                        : ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                                ZLinkRequestFailureMapping.causeCode(failure));
        var completion =
                new CanonicalReply(
                        operationHigh,
                        operationLow,
                        sourceOwnerId,
                        sourceOwnerLeaseGeneration,
                        sourceNodeRid.toString(),
                        sourceNodeGeneration,
                        participant.id(),
                        acceptedSequence,
                        terminal == null ? 0 : terminal[0],
                        terminal == null ? 0 : terminal[1],
                        0,
                        failure == null ? payload : null);
        return relayCanonicalOnce(request, participant, completion)
                .whenComplete(
                        (ignored, relayFailure) -> {
                            if (relayFailure != null
                                    && sourceReplies != null
                                    && sourceReplies.dispatchErrorsEnabled()) {
                                sourceReplies.reportDispatchError(
                                        DispatchFailureReport.of(
                                                        ZLinkDispatchErrorSurface.SPOT_ROUTE,
                                                                ZLinkDispatchMessageKind.RESPONSE,
                                                        ZLinkDispatchErrorReason.REPLY_PATH_MISSING,
                                                                ZLinkDispatchErrorAction
                                                                        .FAIL_CALLER)
                                                .packetName(packetName)
                                                .spotId(request.spotId())
                                                .actorId(
                                                        actor
                                                                ? participant.fence().objectId()
                                                                : null)
                                                .sourceRid(sourceNodeRid)
                                                .correlationId(Long.toString(replyRouteId))
                                                .error(relayFailure));
                            }
                        });
    }

    private CompletionStage<Void> relayCanonicalOnce(
            ZLinkSpotRetireControl.StageRequest request,
            ParticipantSlot participant,
            CanonicalReply completion) {
        var coordinatorFence =
                new ZLinkServiceRelocationWireCodec.CoordinatorFence(
                        request.targetOwnerId(),
                        request.targetOwnerLeaseGeneration(),
                        localNodeRid,
                        localNodeGeneration,
                        request.fence().aggregateId().toString());
        var relay =
                new ZLinkServiceRelocationWireCodec.ReplyRelay(
                        new ZLinkServiceRelocationWireCodec.Operation(
                                completion.operationHigh(), completion.operationLow()),
                        participant.replyRouteId(),
                        new ZLinkServiceRelocationWireCodec.RelocationId(
                                request.fence().aggregateId().getMostSignificantBits(),
                                request.fence().aggregateId().getLeastSignificantBits()),
                        request.fence().aggregateGeneration(),
                        coordinatorFence,
                        participant.id(),
                        completion.sequence(),
                        completion.terminalResult(),
                        completion.failureCode());
        byte[] command = relocationWire.encodeReplyRelay(relay);
        var expectedSource =
                new ZLinkServiceRelocationWireCodec.RequestSourceFence(
                        completion.sourceOwnerId(),
                        completion.sourceOwnerLeaseGeneration(),
                        RoutingId.from(completion.sourceNodeRid()),
                        completion.sourceNodeGeneration());
        //  The reply capability landed on the relocation source node, which
        //  differs from the request-source fence for cross-node accepted
        //  requests. The ACK still closes the exact request-source fence.
        return relocationClient
                .relayCanonicalReply(
                        request.sourceNodeRid(),
                        expectedSource,
                        command,
                        completion.payload() == null
                                ? List.of()
                                : List.of(completion.payload().bytes()),
                        routeTimeout)
                .thenApply(
                        encoded -> {
                            var ack = relocationWire.decodeReplyRelayAck(encoded);
                            validateCanonicalAck(relay, expectedSource, ack);
                            return null;
                        });
    }

    CompletionStage<byte[]> relayCanonicalReply(
            RoutingId transportSource, byte[] command33, List<byte[]> payload) {
        ZLinkServiceRelocationWireCodec.ReplyRelay relay =
                relocationWire.decodeReplyRelay(command33);
        if (sourceReplies == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("relocation reply runtime is unavailable"));
        }
        var route = sourceReplies.canonicalRelocationReplyRoute(relay, transportSource);
        if (route == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("canonical reply capability fence differs"));
        }
        return sourceReplies
                .deliverCanonicalRelocationReply(
                        route, payload, relay.terminalResult(), relay.failureCode())
                .thenApply(
                        ack -> {
                            if (ack == ZLinkSpotRelocationReplyRoutes.Ack.NOT_ACKNOWLEDGED) {
                                throw new IllegalStateException(
                                        "canonical reply capability is busy");
                            }
                            int status =
                                    ack == ZLinkSpotRelocationReplyRoutes.Ack.ALREADY_TERMINAL
                                            ? 2
                                            : 1;
                            return relocationWire.encodeReplyRelayAck(
                                    new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                                            relay.relocation(),
                                            relay.coordinator(),
                                            relay.operation(),
                                            relay.replyRouteId(),
                                            new ZLinkServiceRelocationWireCodec.RequestSourceFence(
                                                    route.sourceOwnerId(),
                                                    route.sourceOwnerLeaseGeneration(),
                                                    route.sourceNodeRid(),
                                                    route.sourceNodeGeneration()),
                                            status));
                        });
    }

    static void validateCanonicalAck(
            ZLinkServiceRelocationWireCodec.ReplyRelay relay,
            ZLinkServiceRelocationWireCodec.RequestSourceFence expectedSource,
            ZLinkServiceRelocationWireCodec.ReplyRelayAck ack) {
        if (!ack.relocation().equals(relay.relocation())
                || !ack.coordinator().equals(relay.coordinator())
                || !ack.operation().equals(relay.operation())
                || ack.replyRouteId() != relay.replyRouteId()
                || !ack.requestSource().ownerId().equals(expectedSource.ownerId())
                || ack.requestSource().leaseGeneration() != expectedSource.leaseGeneration()
                || !ack.requestSource().nodeRid().equals(expectedSource.nodeRid())
                || ack.requestSource().nodeGeneration() != expectedSource.nodeGeneration()) {
            throw new IllegalArgumentException("command 46 fence differs from command 33");
        }
    }

    private static ParticipantSlot participant(
            ZLinkSpotRetireControl.StageRequest request, String objectId, long replyRouteId) {
        for (int index = 0; index < request.participants().size(); index++) {
            var candidate = request.participants().get(index);
            if (candidate.objectId().equals(objectId)) {
                return new ParticipantSlot(index + 1L, candidate, replyRouteId);
            }
        }
        throw new IllegalStateException("relocation participant is absent: " + objectId);
    }

    private static List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant>
            expectedParticipants(ZLinkSpotRetireControl.StageRequest request) {
        return request.participants().stream()
                .map(
                        value ->
                                new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                                        value.authorityKey(),
                                        value.objectGeneration(),
                                        value.sourceAuthorityOwnerGeneration()))
                .toList();
    }

    private static String actorContentType(ZLinkStreamCodec codec) {
        return switch (codec) {
            case RAW -> "application/octet-stream";
            case JSON -> "application/json";
            case MESSAGE_PACK -> "application/msgpack";
            case PROTOBUF -> "application/protobuf";
        };
    }

    private record ParticipantSlot(
            long id, ZLinkSpotRetireControl.ParticipantFence fence, long replyRouteId) {}

    private CompletionStage<Void> switchSessionRoutes(
            ZLinkSpotRetireControl.StageRequest request, Map<String, Long> generations) {
        if (request.sessionRoutes().isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        if (sessionRoutes == null || routeTimeout.isZero() || routeTimeout.isNegative()) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("bound Session route switch runtime is unavailable"));
        }
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (ZLinkSpotRetireControl.SessionRouteFence route : request.sessionRoutes()) {
            String authorityKey =
                    request.participants().stream()
                            .filter(participant -> participant.objectId().equals(route.actorId()))
                            .map(ZLinkSpotRetireControl.ParticipantFence::authorityKey)
                            .findFirst()
                            .orElseThrow(
                                    () ->
                                            new IllegalStateException(
                                                    "session route participant"
                                                            + " is absent: "
                                                            + route.actorId()));
            long targetOwnerGeneration = generations.getOrDefault(authorityKey, 0L);
            var command = routeCommand(request, route, targetOwnerGeneration);
            chain = chain.thenCompose(ignored -> sessionRoutes.sendRoute(command));
        }
        return chain;
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute routeCommand(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkSpotRetireControl.SessionRouteFence route,
            long targetOwnerGeneration) {
        if (targetOwnerGeneration <= 0) {
            throw new IllegalArgumentException("session route target owner generation is invalid");
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
                request.targetOwnerLeaseGeneration());
    }

    @Override
    public CompletionStage<Void> abort(ZLinkSpotRetireControl.StageRequest request) {
        if (isStandaloneActor(request)) {
            return abortActor(request);
        }
        TargetStage target = requireStage(request);
        return staging.discard(target.staged())
                .thenRun(
                        () -> {
                            stages.remove(request.fence(), target);
                        });
    }

    private void validateTarget(ZLinkSpotRetireControl.StageRequest request) {
        if (!request.targetNodeRid().equals(localNodeRid)
                || request.targetNodeGeneration() != localNodeGeneration) {
            throw new IllegalArgumentException("relocation target node fence is stale");
        }
    }

    private CompletionStage<Void> stageActor(ZLinkSpotRetireControl.StageRequest request) {
        if (actorStaging == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("standalone Actor relocation target is unavailable"));
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
        ZLinkActorJoinRelocationPort.Admission directAdmission =
                actorJoin == null ? null : actorJoin.findAdmission(request).orElse(null);
        CompletionStage<ZLinkActorJoinCanonicalAdapter.PreviousMembership> previous =
                directAdmission == null
                        ? CompletableFuture.completedFuture(null)
                        : readPreviousMembership(request, participant);
        return previous.thenCompose(
                previousMembership ->
                        stageStandaloneActor(
                                request,
                                targetRequest,
                                request.relocationPayload(),
                                directAdmission,
                                previousMembership));
    }

    private CompletionStage<Void> publishActor(ZLinkSpotRetireControl.StageRequest request) {
        ActorTargetStage target = requireActorStage(request);
        if (target.directAdmission() != null) {
            target.publishRequested().complete(null);
            return target.joinPublished();
        }
        var participant = request.participants().getFirst();
        return coordinator
                .readTargetOwnerGenerations(
                        expectedParticipants(request),
                        new ZLinkAggregateFence(
                                request.fence().aggregateId(),
                                request.fence().aggregateGeneration()),
                        new ZLinkLocationOwnerToken(
                                request.targetOwnerId(), request.targetOwnerLeaseGeneration()),
                        OPEN)
                .thenCompose(
                        generations -> {
                            long targetOwnerGeneration =
                                    requireGeneration(generations, participant.authorityKey());
                            var backlog =
                                    actorStaging.closeDurableBacklog(
                                            target.staged(),
                                            request.relocationPayload(),
                                            productionActorReplayer(target, request));
                            return actorStaging
                                    .drainDurableBacklog(backlog)
                                    .thenRun(
                                            () -> {
                                                actorStaging.publishHidden(
                                                        backlog, targetOwnerGeneration);
                                            })
                                    .thenCompose(
                                            ignored ->
                                                    boundSessionRestorer.apply(
                                                            request,
                                                            Map.of(
                                                                    participant.objectId(),
                                                                    targetOwnerGeneration)))
                                    .thenCompose(
                                            ignored ->
                                                    switchSessionRoutes(
                                                            target.request(), generations))
                                    .thenCompose(ignored -> normalizer.normalize(request))
                                    .thenRun(() -> actorStaging.openAdmission(target.staged()))
                                    .thenRun(() -> releasePublishedActor(request, target));
                        });
    }

    private CompletionStage<Void> publishDirectJoinActor(
            ZLinkSpotRetireControl.StageRequest request, ActorTargetStage target) {
        var participant = request.participants().getFirst();
        return ZLinkSerialExecutionQueue.yieldCurrent(
                        coordinator.readTargetOwnerGenerations(
                                expectedParticipants(request),
                                new ZLinkAggregateFence(
                                        request.fence().aggregateId(),
                                        request.fence().aggregateGeneration()),
                                new ZLinkLocationOwnerToken(
                                        request.targetOwnerId(),
                                        request.targetOwnerLeaseGeneration()),
                                OPEN))
                .thenCompose(
                        generations -> {
                            long targetOwnerGeneration =
                                    requireGeneration(generations, participant.authorityKey());
                            var replay =
                                    actorStaging.closeDirectJoinIngress(
                                            target.staged(), request.relocationPayload());
                            //  Spec 08-routing §3 step 5: closing the temporary queue
                            //  is the switch to existing Actor dispatch, so the
                            //  admission-time registration that routed arrivals into
                            //  that queue ends here — before command 44 (spec 05 §4.2
                            //  step 8) lets the Session owner relay again. Otherwise the
                            //  first relay after the route switch is handed to the
                            //  closed queue and lost.
                            return actorStaging
                                    .replayDirectJoin(
                                            replay, productionActorReplayer(target, request))
                                    .thenCompose(
                                            terminal -> {
                                                actorJoin.releasePrewarm(
                                                        request.fence().aggregateId());
                                                actorStaging.publishDirectJoinHidden(
                                                        replay, targetOwnerGeneration);
                                                return ZLinkSerialExecutionQueue.yieldCurrent(
                                                                actorStaging
                                                                        .prepareDirectJoinBoundSession(
                                                                                replay,
                                                                                target.request(),
                                                                                targetOwnerGeneration))
                                                        .thenCompose(
                                                                prepared -> {
                                                                    return ZLinkSerialExecutionQueue
                                                                            .manageCurrent(
                                                                                    actorJoin
                                                                                            .notifyTargetJoined(
                                                                                                    target
                                                                                                            .directAdmission(),
                                                                                                    target
                                                                                                            .staged()));
                                                                })
                                                        .handle((ignored, failure) -> failure)
                                                        .thenCompose(
                                                                failure -> {
                                                                    if (failure != null) {
                                                                        return CompletableFuture
                                                                                .completedFuture(
                                                                                        failure);
                                                                    }
                                                                    return ZLinkSerialExecutionQueue
                                                                            .yieldCurrent(
                                                                                    actorJoin
                                                                                            .submitSourceLeave(
                                                                                                    request,
                                                                                                    target
                                                                                                            .previousMembership(),
                                                                                                    targetOwnerGeneration))
                                                                            .thenApply(
                                                                                    ignored ->
                                                                                            failure);
                                                                })
                                                        .thenCompose(
                                                                failure ->
                                                                        ZLinkSerialExecutionQueue
                                                                                .manageCurrent(
                                                                                        actorJoin
                                                                                                .notifyTargetCompletion(
                                                                                                        target
                                                                                                                .directAdmission(),
                                                                                                        target
                                                                                                                .staged(),
                                                                                                        failure))
                                                                                .thenApply(
                                                                                        ignored ->
                                                                                                failure))
                                                        .thenCompose(
                                                                failure -> {
                                                                    if (failure != null) {
                                                                        return CompletableFuture
                                                                                .<Void>failedFuture(
                                                                                        failure);
                                                                    }
                                                                    return CompletableFuture
                                                                            .<Void>completedFuture(
                                                                                    null);
                                                                })
                                                        .thenCompose(
                                                                ignored ->
                                                                        ZLinkSerialExecutionQueue
                                                                                .yieldCurrent(
                                                                                        switchSessionRoutes(
                                                                                                target
                                                                                                        .request(),
                                                                                                generations)))
                                                        .thenCompose(
                                                                ignored ->
                                                                        ZLinkSerialExecutionQueue
                                                                                .yieldCurrent(
                                                                                        normalizer
                                                                                                .normalize(
                                                                                                        request)))
                                                        .thenRun(
                                                                () -> {
                                                                    actorStaging.openAdmission(
                                                                            target.staged());
                                                                    releasePublishedActor(
                                                                            request, target);
                                                                });
                                            });
                        });
    }

    private CompletionStage<ZLinkActorJoinCanonicalAdapter.PreviousMembership>
            readPreviousMembership(
                    ZLinkSpotRetireControl.StageRequest request,
                    ZLinkSpotRetireControl.ParticipantFence participant) {
        if (locations == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "direct Join source authority reader is unavailable"));
        }
        return locations
                .read(participant.authorityKey(), OPEN)
                .thenApply(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                                    || snapshot.objectGeneration() != participant.objectGeneration()
                                    || snapshot.authorityOwnerGeneration()
                                            != participant.sourceAuthorityOwnerGeneration()
                                    || !snapshot.allocation()
                                            .descriptor()
                                            .rid()
                                            .equals(request.sourceNodeRid())
                                    || snapshot.allocation().descriptorLifecycleGeneration()
                                            != request.sourceNodeGeneration()) {
                                throw new IllegalStateException(
                                        "direct Join source membership fence is stale");
                            }
                            var authority =
                                    new ZLinkActorAuthorityPayloadCodec()
                                            .decode(snapshot.payload())
                                            .orElseThrow(
                                                    () ->
                                                            new IllegalStateException(
                                                                    "direct Join source Actor"
                                                                            + " authority is invalid"));
                            return new ZLinkActorJoinCanonicalAdapter.PreviousMembership(
                                    authority.currentSpotId(), authority.currentSpotGeneration());
                        });
    }

    private ZLinkUserSpotAggregateStagingOwner.JournalReplayer productionActorReplayer(
            ActorTargetStage target, ZLinkSpotRetireControl.StageRequest request) {
        if (relocationClient == null) {
            return fallbackReplayer;
        }
        return new ZLinkAcceptedJournalReplayer(
                record ->
                        CompletableFuture.failedFuture(
                                new IllegalArgumentException(
                                        "standalone Actor root contains a Spot journal")),
                record -> actorStaging.replayActor(target.staged(), record),
                new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                    @Override
                    public CompletionStage<Void> completeSpot(
                            ZLinkSpotAcceptedJournal.Record record,
                            long acceptedSequence,
                            List<byte[]> reply,
                            Throwable failure) {
                        return CompletableFuture.failedFuture(
                                new IllegalArgumentException(
                                        "standalone Actor root contains a Spot journal"));
                    }

                    @Override
                    public CompletionStage<Void> completeActor(
                            ZLinkActorAcceptedJournal.Record record,
                            long acceptedSequence,
                            Optional<byte[]> reply,
                            Throwable failure) {
                        CanonicalReplyPayload payload =
                                (reply == null ? Optional.<byte[]>empty() : reply)
                                        .map(
                                                bytes ->
                                                        new CanonicalReplyPayload(
                                                                record.header().name(),
                                                                actorContentType(
                                                                        record.header().codec()),
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
                                payload,
                                failure,
                                record.header().name());
                    }
                });
    }

    private void releasePublishedTarget(
            ZLinkSpotRetireControl.StageRequest request, TargetStage target) {
        if (!stages.remove(request.fence(), target)) {
            throw new IllegalStateException("published relocation target was already released");
        }
    }

    private void releasePublishedActor(
            ZLinkSpotRetireControl.StageRequest request, ActorTargetStage target) {
        if (!actorStages.remove(request.fence(), target)) {
            throw new IllegalStateException(
                    "published standalone Actor target was already released");
        }
    }

    private CompletionStage<Void> abortActor(ZLinkSpotRetireControl.StageRequest request) {
        ActorTargetStage target = requireActorStage(request);
        if (target.directAdmission() != null) {
            target.publishRequested()
                    .completeExceptionally(
                            new IllegalStateException("canonical Actor Join target was aborted"));
        }
        return actorStaging
                .discard(target.staged())
                .thenRun(
                        () -> {
                            actorStages.remove(request.fence(), target);
                            if (target.directAdmission() != null) {
                                //  Release the registry attempt so a later admission
                                //  for the same object does not try to abort an
                                //  already-discarded stage, and so its parked queue
                                //  (empty by now — migration already drained it) is
                                //  not left registered forever.
                                actorJoin.releasePrewarm(request.fence().aggregateId());
                            }
                        });
    }

    private ActorTargetStage requireActorStage(ZLinkSpotRetireControl.StageRequest request) {
        validateTarget(request);
        ActorTargetStage target = actorStages.get(request.fence());
        if (target == null || !target.request().equals(request)) {
            throw new IllegalStateException("standalone Actor target stage is unavailable");
        }
        return target;
    }

    private static long requireGeneration(Map<String, Long> generations, String authorityKey) {
        Long generation = generations.get(authorityKey);
        if (generation == null) {
            throw new IllegalStateException(
                    "target owner generation is absent for authority key: " + authorityKey);
        }
        return generation;
    }

    private static boolean isStandaloneActor(ZLinkSpotRetireControl.StageRequest request) {
        return request.participants().size() == 1
                && request.participants().getFirst().objectKind() == 1;
    }

    private TargetStage requireStage(ZLinkSpotRetireControl.StageRequest request) {
        validateTarget(request);
        TargetStage target = stages.get(request.fence());
        if (target == null || !target.request().equals(request)) {
            throw new IllegalStateException("relocation target stage is unavailable");
        }
        return target;
    }

    private CompletionStage<Void> stageAggregate(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkUserSpotAggregateStagingOwner.Request decoded) {
        return staging.stage(decoded, NOT_CANCELLED)
                .thenCompose(
                        value -> {
                            TargetStage target = new TargetStage(request, value);
                            if (stages.putIfAbsent(request.fence(), target) == null) {
                                return CompletableFuture.<Void>completedFuture(null);
                            }
                            return staging.discard(value)
                                    .thenCompose(
                                            ignored ->
                                                    CompletableFuture.<Void>failedFuture(
                                                            new IllegalStateException(
                                                                    "relocation target stage"
                                                                            + " already exists")));
                        });
    }

    private CompletionStage<Void> stageStandaloneActor(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkStandaloneActorRelocationStagingOwner.Request targetRequest,
            byte[] payload,
            ZLinkActorJoinRelocationPort.Admission directAdmission,
            ZLinkActorJoinCanonicalAdapter.PreviousMembership previousMembership) {
        return actorStaging
                .stage(targetRequest, payload)
                .thenCompose(
                        staged -> {
                            ActorTargetStage target =
                                    new ActorTargetStage(
                                            request,
                                            targetRequest,
                                            staged,
                                            directAdmission,
                                            previousMembership,
                                            new CompletableFuture<>(),
                                            new CompletableFuture<>());
                            if (directAdmission == null) {
                                if (actorStages.putIfAbsent(request.fence(), target) == null) {
                                    return CompletableFuture.<Void>completedFuture(null);
                                }
                                return actorStaging
                                        .discard(staged)
                                        .thenCompose(
                                                ignored ->
                                                        CompletableFuture.<Void>failedFuture(
                                                                new IllegalStateException(
                                                                        "standalone Actor target"
                                                                                + " stage already "
                                                                                + "exists")));
                            }
                            //  Atomic transition (spec 15 §4.2): install the real
                            //  stage, migrate every arrival parked since Accepted
                            //  into it in order, then the placeholder attempt is
                            //  live — all under the prewarm registry's monitor, so
                            //  ingress never observes a window where the object is
                            //  neither parkable nor deliverable. Throws if a newer
                            //  exact identity for the same object already evicted
                            //  this attempt (spec 15 §4.2 "이전 identity의 늦은 chunk와
                            //  Restore는 조립에 연결하지 않고 폐기한다").
                            UUID relocationId = request.fence().aggregateId();
                            DefaultSpotContext context =
                                    (DefaultSpotContext)
                                            ((ZLinkSpot<?>) directAdmission.targetSpot()).context();
                            context.enqueueLifecycle(
                                            () ->
                                                    ZLinkSerialExecutionQueue.yieldCurrent(
                                                                    target.publishRequested())
                                                            .thenCompose(
                                                                    ignored ->
                                                                            publishDirectJoinActor(
                                                                                    request,
                                                                                    target)))
                                    .whenComplete(
                                            (ignored, failure) -> {
                                                if (failure == null) {
                                                    target.joinPublished().complete(null);
                                                } else {
                                                    target.joinPublished()
                                                            .completeExceptionally(failure);
                                                }
                                            });
                            try {
                                actorJoin.completeMigration(
                                        relocationId,
                                        parked ->
                                                actorStaging.acceptIngress(
                                                        target.staged(),
                                                        parked.record(),
                                                        parked.reply(),
                                                        parked.failure()),
                                        () -> {
                                            if (actorStages.putIfAbsent(request.fence(), target)
                                                    != null) {
                                                throw new IllegalStateException(
                                                        "standalone Actor target stage already "
                                                                + "exists");
                                            }
                                        },
                                        () -> {
                                            //  Newer exact identity evicted this attempt
                                            //  before publish committed: tear the
                                            //  installed (not yet published) stage down.
                                            //  Never block on the discard here — the
                                            //  registry monitor must stay non-blocking.
                                            if (actorStages.remove(request.fence(), target)) {
                                                target.publishRequested()
                                                        .completeExceptionally(
                                                                new IllegalStateException(
                                                                        "canonical Actor Join"
                                                                                + " target was"
                                                                                + " superseded"));
                                                actorStaging
                                                        .discard(target.staged())
                                                        .exceptionally(
                                                                failure -> {
                                                                    LOGGER.warning(
                                                                            "Actor Join newest-wins"
                                                                                    + " eviction failed"
                                                                                    + " to discard the"
                                                                                    + " displaced"
                                                                                    + " stage: "
                                                                                    + failure);
                                                                    return null;
                                                                });
                                            }
                                        });
                            } catch (
                                    ZLinkActorJoinPrewarmRegistry.SupersededAttemptException
                                            superseded) {
                                target.publishRequested().completeExceptionally(superseded);
                                actorJoin.releasePrewarm(relocationId);
                                return actorStaging
                                        .discard(staged)
                                        .thenCompose(
                                                ignored ->
                                                        CompletableFuture.<Void>failedFuture(
                                                                ZLinkFrameworkErrorOrigin.framework(
                                                                        ZLinkFrameworkErrorKind
                                                                                .UNAVAILABLE,
                                                                        "canonical Actor Join"
                                                                                + " PREPARE was"
                                                                                + " superseded by a"
                                                                                + " newer relocation"
                                                                                + " identity",
                                                                        superseded)));
                            } catch (RuntimeException failure) {
                                target.publishRequested().completeExceptionally(failure);
                                throw failure;
                            }
                            return CompletableFuture.completedFuture(null);
                        });
    }

    @FunctionalInterface
    interface SteadyNormalizer {
        CompletionStage<Void> normalize(ZLinkSpotRetireControl.StageRequest request);
    }

    private record TargetStage(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkUserSpotAggregateStagingOwner.Staged staged) {}

    private record ActorTargetStage(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkStandaloneActorRelocationStagingOwner.Request targetRequest,
            ZLinkStandaloneActorRelocationStagingOwner.Staged staged,
            ZLinkActorJoinRelocationPort.Admission directAdmission,
            ZLinkActorJoinCanonicalAdapter.PreviousMembership previousMembership,
            CompletableFuture<Void> publishRequested,
            CompletableFuture<Void> joinPublished) {}

    private record CanonicalReply(
            long operationHigh,
            long operationLow,
            String sourceOwnerId,
            long sourceOwnerLeaseGeneration,
            String sourceNodeRid,
            long sourceNodeGeneration,
            long participantId,
            long sequence,
            int terminalResult,
            int failureCode,
            int deliveryState,
            CanonicalReplyPayload payload) {}

    private record CanonicalReplyPayload(String packetName, String contentType, byte[] bytes) {
        private CanonicalReplyPayload {
            Objects.requireNonNull(packetName, "packetName");
            Objects.requireNonNull(contentType, "contentType");
            bytes = Objects.requireNonNull(bytes, "bytes").clone();
        }

        @Override
        public byte[] bytes() {
            return bytes.clone();
        }
    }
}
