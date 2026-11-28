package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
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
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator.TargetIngressAuthority;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationReplyRow;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationReplyRow.Source;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationEnvelopeCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import systems.zlink.framework.spots.ZLinkSpot;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HexFormat;
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
    final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkUserSpotAggregateStagingOwner staging;
    private final BiFunction<ZLinkPlacementObjectKind, String, Class<?>> spotTypes;
    private final ZLinkUserSpotAggregateStagingOwner.JournalReplayer fallbackReplayer;
    private final ZLinkSessionRelocationPeerClient sessionRoutes;
    private final Duration routeTimeout;
    final SteadyNormalizer normalizer;
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
                (kind, stableType) -> spotTypes.apply(stableType),
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
                (kind, stableType) -> spotTypes.apply(stableType),
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
            BiFunction<ZLinkPlacementObjectKind, String, Class<?>> spotTypes,
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
    public CompletionStage<ZLinkSpotRetireControl.TargetProfile> applyTargetProfile(
            ZLinkSpotRetireControl.TargetProfile profile,
            Supplier<CompletionStage<Void>> awaitShellPublication) {
        var request = profile.request();
        if (!isStandaloneActor(request)) return CompletableFuture.completedFuture(profile);

        if (actorJoin != null) {
            actorJoin.claimRecovery(request);
            var admittedJoin = actorJoin.admittedTargetProfile(request);
            if (admittedJoin.isPresent()) {
                return CompletableFuture.completedFuture(admittedJoin.orElseThrow());
            }
        }
        if (profile.actorSpotKind() != 2) return CompletableFuture.completedFuture(profile);

        return awaitShellPublication
                .get()
                .thenApply(
                        ignored -> {
                            SpotActivation shell =
                                    sourceReplies == null
                                            ? null
                                            : sourceReplies
                                                    .spotLifecycle()
                                                    .spotActivationFor(request.spotId());
                            if (shell == null
                                    || shell.context.executionMode()
                                            != ZLinkUserSpotExecutionMode.PER_ACTOR
                                    || shell.backendSpot.lifecycleGeneration()
                                            != profile.actorSpotGeneration()
                                    || !shell.context.nodeRid().equals(localNodeRid)) {
                                throw new IllegalStateException(
                                        "Actor relocation target shell differs");
                            }
                            return profile;
                        });
    }

    @Override
    public CompletionStage<Void> stage(ZLinkSpotRetireControl.StageRequest request) {
        return stage(request, null);
    }

    @Override
    public CompletionStage<Void> stage(
            ZLinkSpotRetireControl.StageRequest request, TargetIngressAuthority authority) {
        validateTarget(request);
        if (isStandaloneActor(request)) {
            return stageActor(request, authority);
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
        return stageAggregate(request, decoded, authority);
    }

    @Override
    public ZLinkTargetRelocationReplies prepareCompletions(
            ZLinkSpotRetireControl.StageRequest request,
            List<byte[]> verifiedBatch,
            Consumer<Runnable> resubmit) {
        List<ZLinkRelocationReplyRow> accepted = new ArrayList<>();
        var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(request.relocationPayload());
        for (var work : envelope.savedWork()) {
            captureRequest(request, work.frozenRecord(), accepted);
        }
        for (byte[] record : verifiedBatch) {
            captureRequest(request, record, accepted);
        }
        return new ZLinkTargetRelocationReplies(
                request, coordinator, locations, resubmit, accepted);
    }

    private void captureRequest(
            ZLinkSpotRetireControl.StageRequest request,
            byte[] frozen,
            List<ZLinkRelocationReplyRow> accepted) {
        if (!ZLinkServiceFrozenRecordCodec.mayHaveReplyRoute(frozen)) return;
        if (ZLinkServiceFrozenRecordCodec.isActorRecord(frozen)) {
            var record = ZLinkActorAcceptedJournal.decode(frozen);
            record.replyRouteId()
                    .ifPresent(
                            route ->
                                    accepted.add(
                                            acceptedRow(
                                                    request,
                                                    participant(
                                                            request, record.actorId(), true, route),
                                                    record.operationHigh(),
                                                    record.operationLow(),
                                                    record.sourceOwnerId(),
                                                    record.sourceOwnerLeaseGeneration(),
                                                    record.sourceNodeRid(),
                                                    record.sourceNodeGeneration())));
        } else {
            var record = ZLinkSpotAcceptedJournal.decode(frozen);
            record.requestSequence()
                    .ifPresent(
                            route ->
                                    accepted.add(
                                            acceptedRow(
                                                    request,
                                                    participant(
                                                            request,
                                                            request.spotId(),
                                                            false,
                                                            route),
                                                    record.operationHigh(),
                                                    record.operationLow(),
                                                    record.sourceOwnerId(),
                                                    record.sourceOwnerLeaseGeneration(),
                                                    record.routingId().orElseThrow(),
                                                    record.sourceNodeGeneration())));
        }
    }

    private ZLinkRelocationReplyRow acceptedRow(
            ZLinkSpotRetireControl.StageRequest request,
            ParticipantSlot participant,
            long operationHigh,
            long operationLow,
            String sourceOwner,
            long sourceLease,
            RoutingId sourceNode,
            long sourceGeneration) {
        return replyRow(
                request,
                participant,
                operationHigh,
                operationLow,
                sourceOwner,
                sourceLease,
                sourceNode,
                sourceGeneration,
                ZLinkRelocationReplyRow.ACCEPTED,
                null);
    }

    @Override
    public CompletionStage<Void> publish(
            ZLinkSpotRetireControl.StageRequest request, ZLinkTargetRelocationReplies completions) {
        if (isStandaloneActor(request)) {
            return publishActor(request, completions);
        }
        TargetStage target = requireStage(request);
        return coordinator
                .readTargetAuthorities(
                        expectedParticipants(request),
                        new ZLinkAggregateFence(
                                request.fence().aggregateId(),
                                request.fence().aggregateGeneration()),
                        new ZLinkLocationOwnerToken(
                                request.targetOwnerId(), request.targetOwnerLeaseGeneration()),
                        OPEN)
                .thenCompose(
                        snapshots -> {
                            Map<String, Long> generations = new HashMap<>();
                            snapshots.forEach(
                                    (key, snapshot) ->
                                            generations.put(
                                                    key, snapshot.authorityOwnerGeneration()));
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
                            staging.setCommittedSpotAuthority(
                                    target.staged(),
                                    snapshots.get(ZLinkAuthorityKeyCodec.spot(request.spotId())));
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
                                            productionReplayer(target, request, completions))
                                    .thenCompose(
                                            backlog -> {
                                                return staging.publishHidden(
                                                                backlog, actorOwnerGenerations)
                                                        .thenRun(
                                                                () -> {
                                                                    if (sourceReplies == null)
                                                                        return;
                                                                    for (var participant :
                                                                            request
                                                                                    .participants()) {
                                                                        if (participant.objectKind()
                                                                                != ZLinkPlacementObjectKind
                                                                                        .ACTOR
                                                                                        .value()) {
                                                                            sourceReplies
                                                                                    .rememberRecoveredSpot(
                                                                                            snapshots
                                                                                                    .get(
                                                                                                            participant
                                                                                                                    .authorityKey()),
                                                                                            participant
                                                                                                    .objectId());
                                                                        }
                                                                    }
                                                                })
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
                                    .thenCompose(
                                            ignored ->
                                                    openAndNormalize(
                                                            request,
                                                            () ->
                                                                    staging.openAdmission(
                                                                                    target.staged())
                                                                            .thenRun(
                                                                                    () ->
                                                                                            releasePublishedTarget(
                                                                                                    request,
                                                                                                    target))));
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
        boolean actor = ZLinkServiceFrozenRecordCodec.isActorRecord(frozenRecord);
        String objectId =
                actor ? ZLinkActorAcceptedJournal.decode(frozenRecord).actorId() : request.spotId();
        return staging.stageRelayedRecord(
                requireStage(request).staged(), objectId, actor, frozenRecord);
    }

    boolean hasTemporarySpotIngress(String spotId) {
        return stages.values().stream()
                .anyMatch(stage -> stage.staged().hasTemporarySpotIngress(spotId));
    }

    @Override
    public boolean handleInstanceSpotFollow(
            Supplier<CompletionStage<Optional<ZLinkInternalMeshNode.PeerAuthorityFence>>>
                    sourceAuthority,
            ZLinkServiceM6BWireCodec.InstanceSpotFollow follow,
            byte[] metadata,
            byte[] payload,
            Consumer<Throwable> failure) {
        var route = follow.target();
        var ingress =
                new IngressRoute(
                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                        route.spotId(),
                        route.spotGeneration(),
                        route.targetNodeRid(),
                        route.targetNodeGeneration(),
                        route.authorityOwnerGeneration(),
                        route.ownerLeaseGeneration(),
                        true);
        for (TargetStage target : stages.values()) {
            var participant =
                    ingress.participant(target.request(), null, target.authority() != null);
            if (participant == null) continue;
            var confirmation = ingress.confirmation(participant);
            // Resolve the original sender without waiting. The gate holds this queue position
            // until both the sender and the committed target have been confirmed.
            var source = sourceAuthority.get().toCompletableFuture();
            if (!staging.acceptSpotIngress(
                    target.staged(),
                    ready ->
                            ZLinkServiceFrozenRecordCodec.encodeInstanceSpot(
                                    source.getNow(Optional.empty()).orElseThrow(),
                                    follow.readyMessage(ready),
                                    ready,
                                    metadata,
                                    payload),
                    (long) metadata.length
                            + payload.length
                            + ZLinkServiceFrozenRecordCodec.ACCEPTED_RECORD_OVERHEAD_BYTES,
                    null,
                    failure,
                    confirmation)) {
                return false;
            }
            confirmIngress(
                    ingress,
                    follow.sourceNodeRid(),
                    confirmation,
                    failure,
                    null,
                    source.thenCompose(
                            resolved ->
                                    resolved.isPresent()
                                            ? confirmTargetIngress(
                                                    target.authority(),
                                                    target.request(),
                                                    participant,
                                                    ingress)
                                            : CompletableFuture.completedFuture(false)));
            return true;
        }
        return false;
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
        return handleSpot(
                source,
                header,
                metadata,
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                parts,
                contentType,
                reply,
                failure,
                null);
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
            Consumer<Throwable> failure,
            Runnable admitted) {
        var route = header.target();
        var ingress =
                new IngressRoute(
                        ZLinkPlacementObjectKind.USER_SPOT,
                        route.spotId(),
                        route.spotGeneration(),
                        route.targetNodeRid(),
                        route.targetNodeGeneration(),
                        route.authorityOwnerGeneration(),
                        route.ownerLeaseGeneration(),
                        header.request() && header.messageFollowHopCount() == 0);
        for (TargetStage target : stages.values()) {
            var participant =
                    ingress.participant(target.request(), source, target.authority() != null);
            if (participant == null) continue;
            var confirmation = ingress.confirmation(participant);
            if (!staging.acceptSpotIngress(
                    target.staged(), acceptedJournalRecord, reply, failure, confirmation)) {
                return false;
            }
            parts.forEach(Message::close);
            confirmIngress(
                    target.authority(),
                    target.request(),
                    participant,
                    ingress,
                    source,
                    confirmation,
                    failure,
                    admitted);
            return true;
        }
        return false;
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
        return handleActor(
                source, header, acceptedJournalRecord, parts, contentType, reply, failure, null);
    }

    @Override
    public boolean handleActor(
            ZLinkInternalMeshNode.PeerAuthorityFence source,
            ZLinkServiceM6BWireCodec.ActorMessage header,
            Supplier<byte[]> acceptedJournalRecord,
            List<Message> parts,
            String contentType,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure,
            Runnable admitted) {
        var route = header.target();
        var ingress =
                new IngressRoute(
                        ZLinkPlacementObjectKind.ACTOR,
                        route.actor().actorId(),
                        route.actor().generation(),
                        route.actor().nodeRid(),
                        route.targetNodeGeneration(),
                        route.authorityOwnerGeneration(),
                        route.ownerLeaseGeneration(),
                        header.request()
                                && header.messageFollowHopCount() == 0
                                && header.boundSession() == null);
        for (ActorTargetStage target : actorStages.values()) {
            var participant =
                    ingress.participant(
                            target.request(),
                            source,
                            target.authority() != null && target.directAdmission() == null);
            if (participant == null) continue;
            var confirmation = ingress.confirmation(participant);
            if (!actorStaging.acceptIngress(
                    target.staged(), acceptedJournalRecord, reply, failure, confirmation)) {
                return false;
            }
            parts.forEach(Message::close);
            confirmIngress(
                    target.authority(),
                    target.request(),
                    participant,
                    ingress,
                    source,
                    confirmation,
                    failure,
                    admitted);
            return true;
        }
        for (TargetStage target : stages.values()) {
            var participant =
                    ingress.participant(target.request(), source, target.authority() != null);
            if (participant == null) continue;
            var confirmation = ingress.confirmation(participant);
            if (confirmation == null
                    && acceptJoinIngress(
                            header, acceptedJournalRecord, parts, reply, failure, admitted)) {
                return true;
            }
            if (!staging.acceptActorIngress(
                    target.staged(),
                    ingress.objectId(),
                    acceptedJournalRecord,
                    reply,
                    failure,
                    confirmation)) {
                return false;
            }
            parts.forEach(Message::close);
            confirmIngress(
                    target.authority(),
                    target.request(),
                    participant,
                    ingress,
                    source,
                    confirmation,
                    failure,
                    admitted);
            return true;
        }
        return acceptJoinIngress(header, acceptedJournalRecord, parts, reply, failure, admitted);
    }

    private boolean acceptJoinIngress(
            ZLinkServiceM6BWireCodec.ActorMessage header,
            Supplier<byte[]> acceptedJournalRecord,
            List<Message> parts,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure,
            Runnable admitted) {
        if (actorJoin == null
                || !tryRouteActorIngressToPrewarm(header, acceptedJournalRecord, reply, failure)) {
            return false;
        }
        parts.forEach(Message::close);
        if (admitted != null) admitted.run();
        return true;
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

    private void confirmIngress(
            TargetIngressAuthority authority,
            ZLinkSpotRetireControl.StageRequest stage,
            ZLinkSpotRetireControl.ParticipantFence participant,
            IngressRoute ingress,
            ZLinkInternalMeshNode.PeerAuthorityFence source,
            CompletableFuture<Boolean> confirmation,
            Consumer<Throwable> failure,
            Runnable admitted) {
        if (confirmation == null) {
            if (admitted != null) admitted.run();
            return;
        }
        confirmIngress(
                ingress,
                source.sourceNodeRid(),
                confirmation,
                failure,
                admitted,
                confirmTargetIngress(authority, stage, participant, ingress));
    }

    private static void confirmIngress(
            IngressRoute ingress,
            RoutingId sourceNodeRid,
            CompletableFuture<Boolean> confirmation,
            Consumer<Throwable> failure,
            Runnable admitted,
            CompletionStage<Boolean> read) {
        read.whenComplete(
                (confirmed, readFailure) -> {
                    if (readFailure != null || !Boolean.TRUE.equals(confirmed)) {
                        try {
                            if (failure != null)
                                failure.accept(ingress.rejection(sourceNodeRid, readFailure));
                        } finally {
                            confirmation.complete(false);
                        }
                    } else {
                        try {
                            if (admitted != null) admitted.run();
                        } finally {
                            confirmation.complete(true);
                        }
                    }
                });
    }

    private CompletionStage<Boolean> confirmTargetIngress(
            TargetIngressAuthority authority,
            ZLinkSpotRetireControl.StageRequest stage,
            ZLinkSpotRetireControl.ParticipantFence participant,
            IngressRoute ingress) {
        try {
            return coordinator.confirmTargetIngress(
                    authority,
                    new ZLinkAggregateFence(
                            stage.fence().aggregateId(), stage.fence().aggregateGeneration()),
                    new ZLinkLocationOwnerToken(
                            stage.targetOwnerId(), stage.targetOwnerLeaseGeneration()),
                    new ZLinkLocationOwnerToken(
                            stage.sourceOwnerId(), stage.sourceOwnerLeaseGeneration()),
                    new ZLinkMeshNodeDescriptorKey(stage.meshName(), stage.targetNodeRid()),
                    stage.targetNodeGeneration(),
                    stage.sourceNodeRid(),
                    stage.sourceNodeGeneration(),
                    new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                            participant.authorityKey(),
                            participant.objectGeneration(),
                            participant.sourceAuthorityOwnerGeneration()),
                    ingress.authorityGeneration(),
                    OPEN);
        } catch (RuntimeException rejected) {
            return CompletableFuture.failedFuture(rejected);
        }
    }

    private record IngressRoute(
            ZLinkPlacementObjectKind kind,
            String objectId,
            long objectGeneration,
            RoutingId nodeRid,
            long nodeGeneration,
            long authorityGeneration,
            long ownerLeaseGeneration,
            boolean acceptsCommittedIngress) {
        ZLinkSpotRetireControl.ParticipantFence participant(
                ZLinkSpotRetireControl.StageRequest stage,
                ZLinkInternalMeshNode.PeerAuthorityFence source,
                boolean acceptDirect) {
            if (!stage.targetNodeRid().equals(nodeRid)
                    || stage.targetNodeGeneration() != nodeGeneration
                    || stage.targetOwnerLeaseGeneration() != ownerLeaseGeneration) return null;
            for (var member : stage.participants()) {
                if (matchesKind(member.objectKind())
                        && member.objectId().equals(objectId)
                        && member.objectGeneration() == objectGeneration
                        && (acceptDirect
                                        && acceptsCommittedIngress
                                        && authorityGeneration
                                                > member.sourceAuthorityOwnerGeneration()
                                || source != null
                                        && authorityGeneration
                                                == member.sourceAuthorityOwnerGeneration()
                                        && stage.sourceNodeRid().equals(source.sourceNodeRid())
                                        && stage.sourceNodeGeneration()
                                                == source.sourceNodeGeneration()
                                        && stage.sourceOwnerId().equals(source.ownerId())
                                        && stage.sourceOwnerLeaseGeneration()
                                                == source.ownerLeaseGeneration())) {
                    return member;
                }
            }
            return null;
        }

        private boolean matchesKind(int objectKind) {
            return kind == ZLinkPlacementObjectKind.USER_SPOT
                    ? objectKind != ZLinkPlacementObjectKind.ACTOR.value()
                    : objectKind == kind.value();
        }

        CompletableFuture<Boolean> confirmation(
                ZLinkSpotRetireControl.ParticipantFence participant) {
            return authorityGeneration == participant.sourceAuthorityOwnerGeneration()
                    ? null
                    : new CompletableFuture<>();
        }

        Throwable rejection(RoutingId sourceNodeRid, Throwable cause) {
            boolean local = nodeRid.equals(sourceNodeRid);
            var failureKind =
                    local
                            ? ZLinkFrameworkErrorKind.NOT_FOUND
                            : kind == ZLinkPlacementObjectKind.ACTOR
                                    ? ZLinkFrameworkErrorKind.UNAVAILABLE
                                    : ZLinkFrameworkErrorKind.INVALID_OPERATION;
            int code =
                    (int)
                            (kind == ZLinkPlacementObjectKind.ACTOR
                                    ? local
                                            ? ServiceWireConstants
                                                    .FRAMEWORK_ERROR_ACTOR_ROUTE_NOT_FOUND
                                            : ServiceWireConstants
                                                    .FRAMEWORK_ERROR_ACTOR_LOCATION_STALE
                                    : local
                                            ? ServiceWireConstants
                                                    .FRAMEWORK_ERROR_SPOT_ROUTE_NOT_FOUND
                                            : ServiceWireConstants
                                                    .FRAMEWORK_ERROR_SPOT_GENERATION_STALE);
            return ZLinkRequestFailureMapping.receivedFailure(
                    failureKind,
                    "target relocation authority does not match the request",
                    code,
                    ZLinkFrameworkErrorOrigin.frameworkMetadata(),
                    cause);
        }
    }

    private ZLinkUserSpotAggregateStagingOwner.JournalReplayer productionReplayer(
            TargetStage target,
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkTargetRelocationReplies completions) {
        if (relocationClient == null) {
            return fallbackReplayer;
        }
        return new ZLinkAcceptedJournalReplayer(
                record -> staging.replaySpot(target.staged(), record),
                record -> staging.replayActor(target.staged(), record),
                productionReplyRelay(request, completions));
    }

    ZLinkAcceptedJournalReplayer.ReplyRelay productionReplyRelay(
            ZLinkSpotRetireControl.StageRequest request, ZLinkTargetRelocationReplies completions) {
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
                        participant(
                                request,
                                request.spotId(),
                                false,
                                record.requestSequence().orElse(0L));
                CanonicalReplyPayload payload = null;
                if (failure == null && record.requestSequence().isPresent()) {
                    if (reply.isEmpty() || record.parts().isEmpty()) {
                        return CompletableFuture.failedFuture(
                                new IllegalStateException(
                                        "canonical Spot request must produce a reply"));
                    }
                    if (reply.size() == 1) {
                        payload =
                                new CanonicalReplyPayload(
                                        new String(
                                                record.parts().getFirst(), StandardCharsets.UTF_8),
                                        "application/zlink-framework-json-v1",
                                        reply.getFirst());
                    } else {
                        var parts = reply.stream().map(Message::from).toList();
                        try {
                            var multipart =
                                    ZLinkServiceM6AWireCodec.encodeFrameworkMultipart(parts);
                            payload =
                                    new CanonicalReplyPayload(
                                            multipart.packetName(),
                                            multipart.contentType(),
                                            multipart.payload());
                        } finally {
                            parts.forEach(Message::close);
                        }
                    }
                }
                return completeCanonicalReplay(
                        request,
                        completions,
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
                                .map(bytes -> canonicalActorReply(record, bytes))
                                .orElse(null);
                return completeCanonicalReplay(
                        request,
                        completions,
                        participant(
                                request, record.actorId(), true, record.replyRouteId().orElse(0L)),
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

    private static CanonicalReplyPayload canonicalActorReply(
            ZLinkActorAcceptedJournal.Record record, byte[] bytes) {
        var request = record.header();
        try (var message = Message.from(bytes);
                var frame =
                        ActorPacketFrames.encodeRoutedReply(
                                request,
                                new systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime
                                        .LocalActorReply(message, request.codec()))) {
            return new CanonicalReplyPayload(
                    request.name(), "application/octet-stream", frame.toByteArray());
        }
    }

    private CompletionStage<Void> completeCanonicalReplay(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkTargetRelocationReplies completions,
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
                        sourceNodeRid,
                        sourceNodeGeneration,
                        acceptedSequence,
                        terminal == null ? 0 : terminal[0],
                        terminal == null ? 0 : terminal[1],
                        failure == null ? payload : null);
        var delivery =
                completions.complete(
                        entry(request, participant, completion),
                        () ->
                                entry(
                                        request,
                                        participant,
                                        new CanonicalReply(
                                                operationHigh,
                                                operationLow,
                                                sourceOwnerId,
                                                sourceOwnerLeaseGeneration,
                                                sourceNodeRid,
                                                sourceNodeGeneration,
                                                acceptedSequence,
                                                RequestResult.INTERNAL_ERROR.value(),
                                                (int)
                                                        ServiceWireConstants
                                                                .FRAMEWORK_ERROR_REQUEST_FAILED,
                                                null)),
                        this::sendStored);
        return delivery.whenComplete(
                (ignored, relayFailure) -> {
                    if (relayFailure != null
                            && sourceReplies != null
                            && sourceReplies.dispatchErrorsEnabled()) {
                        sourceReplies.reportDispatchError(
                                DispatchFailureReport.of(
                                                ZLinkDispatchErrorSurface.SPOT_ROUTE,
                                                        ZLinkDispatchMessageKind.RESPONSE,
                                                ZLinkDispatchErrorReason.REPLY_PATH_MISSING,
                                                        ZLinkDispatchErrorAction.FAIL_CALLER)
                                        .packetName(packetName)
                                        .spotId(request.spotId())
                                        .actorId(actor ? participant.fence().objectId() : null)
                                        .sourceRid(sourceNodeRid)
                                        .correlationId(Long.toString(replyRouteId))
                                        .error(relayFailure));
                    }
                });
    }

    private ZLinkServiceRelocationWireCodec.ReplyRelay canonicalRelay(
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
        return new ZLinkServiceRelocationWireCodec.ReplyRelay(
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
    }

    private ZLinkRelocationReplyRow entry(
            ZLinkSpotRetireControl.StageRequest request,
            ParticipantSlot participant,
            CanonicalReply result) {
        var payload =
                result.payload() == null
                        ? null
                        : new ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1(
                                result.payload().packetName(),
                                result.payload().contentType(),
                                result.payload().bytes());
        return replyRow(
                request,
                participant,
                result.operationHigh(),
                result.operationLow(),
                result.sourceOwnerId(),
                result.sourceOwnerLeaseGeneration(),
                result.sourceNodeRid(),
                result.sourceNodeGeneration(),
                ZLinkRelocationReplyRow.PENDING,
                relocationWire.encodeStoredReply(
                        canonicalRelay(request, participant, result), payload));
    }

    private static ZLinkRelocationReplyRow replyRow(
            ZLinkSpotRetireControl.StageRequest request,
            ParticipantSlot participant,
            long operationHigh,
            long operationLow,
            String sourceOwner,
            long sourceLease,
            RoutingId sourceNode,
            long sourceGeneration,
            String state,
            byte[] reply) {
        return new ZLinkRelocationReplyRow(
                request.fence().aggregateId(),
                participant.fence().authorityKey(),
                new Source(
                        sourceOwner,
                        Long.toUnsignedString(sourceLease),
                        sourceNode.toHex(),
                        Long.toUnsignedString(sourceGeneration)),
                HexFormat.of().toHexDigits(operationHigh)
                        + HexFormat.of().toHexDigits(operationLow),
                Long.toUnsignedString(participant.replyRouteId()),
                new ZLinkLocationOwnerToken(
                        request.targetOwnerId(), request.targetOwnerLeaseGeneration()),
                state,
                reply);
    }

    private CompletionStage<String> sendStored(ZLinkRelocationReplyRow entry) {
        var stored = relocationWire.decodeStoredReply(entry.reply());
        var source = entry.source();
        var frames = relocationWire.encodeReplyRelayFrames(stored.relay(), stored.payload());
        var expectedSource =
                new ZLinkServiceRelocationWireCodec.RequestSourceFence(
                        source.ownerId(),
                        Long.parseUnsignedLong(source.leaseGeneration()),
                        RoutingId.fromHex(source.nodeRid()),
                        Long.parseUnsignedLong(source.nodeGeneration()));
        return relocationClient
                .relayCanonicalReply(
                        expectedSource.nodeRid(),
                        expectedSource,
                        frames.getFirst(),
                        frames.subList(1, frames.size()),
                        routeTimeout)
                .thenApply(
                        encoded -> {
                            var ack = relocationWire.decodeReplyRelayAck(encoded);
                            return ack.status() == 1
                                    ? ZLinkRelocationReplyRow.TERMINAL_RECEIVED
                                    : ZLinkRelocationReplyRow.ALREADY_TERMINAL;
                        });
    }

    private static ParticipantSlot participant(
            ZLinkSpotRetireControl.StageRequest request,
            String objectId,
            boolean actor,
            long replyRouteId) {
        for (int index = 0; index < request.participants().size(); index++) {
            var candidate = request.participants().get(index);
            if (candidate.objectId().equals(objectId)
                    && (candidate.objectKind() == ZLinkPlacementObjectKind.ACTOR.value())
                            == actor) {
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

    private CompletionStage<Void> stageActor(
            ZLinkSpotRetireControl.StageRequest request, TargetIngressAuthority authority) {
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
                                authority,
                                directAdmission,
                                previousMembership));
    }

    private CompletionStage<Void> publishActor(
            ZLinkSpotRetireControl.StageRequest request, ZLinkTargetRelocationReplies completions) {
        ActorTargetStage target = requireActorStage(request);
        if (target.directAdmission() != null) {
            target.publishRequested().complete(completions);
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
                                            productionActorReplayer(target, request, completions));
                            return actorStaging
                                    .drainAndPublish(backlog, targetOwnerGeneration)
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
                                    .thenCompose(ignored -> openPublishedActor(request, target));
                        });
    }

    private CompletionStage<Void> publishDirectJoinActor(
            ZLinkSpotRetireControl.StageRequest request,
            ActorTargetStage target,
            ZLinkTargetRelocationReplies completions) {
        var participant = request.participants().getFirst();
        return ZLinkSerialExecutionQueue.yieldCurrent(
                        coordinator.readTargetOwnerGenerations(
                                expectedParticipants(request),
                                ZLinkAggregateFence.markerlessRelocation(
                                        request.fence().aggregateId()),
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
                            // Prefix admission may post to this gate after Actor preparation.
                            return ZLinkSerialExecutionQueue.yieldCurrent(
                                            actorStaging.replayDirectJoin(
                                                    replay,
                                                    productionActorReplayer(
                                                            target, request, completions)))
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
                                                                    actorJoin.submitSourceLeave(
                                                                            request,
                                                                            target
                                                                                    .previousMembership(),
                                                                            targetOwnerGeneration);
                                                                    return CompletableFuture
                                                                            .completedFuture(
                                                                                    failure);
                                                                })
                                                        .thenCompose(
                                                                failure ->
                                                                        openAndNormalize(
                                                                                request,
                                                                                () ->
                                                                                        finishDirectJoinPublication(
                                                                                                request,
                                                                                                target,
                                                                                                generations,
                                                                                                failure)));
                                            });
                        });
    }

    private CompletionStage<Void> finishDirectJoinPublication(
            ZLinkSpotRetireControl.StageRequest request,
            ActorTargetStage target,
            Map<String, Long> generations,
            Throwable joinedFailure) {
        return actorJoin
                .notifyTargetCompletion(target.directAdmission(), target.staged(), joinedFailure)
                .thenCompose(
                        ignored ->
                                joinedFailure == null
                                        ? ZLinkSerialExecutionQueue.yieldCurrent(
                                                switchSessionRoutes(target.request(), generations))
                                        : CompletableFuture.failedFuture(joinedFailure))
                .thenRun(() -> openActorAdmission(request, target));
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
            ActorTargetStage target,
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkTargetRelocationReplies completions) {
        if (relocationClient == null) {
            return fallbackReplayer;
        }
        return new ZLinkAcceptedJournalReplayer(
                record ->
                        CompletableFuture.failedFuture(
                                new IllegalArgumentException(
                                        "standalone Actor root contains a Spot journal")),
                record -> actorStaging.replayActor(target.staged(), record),
                productionReplyRelay(request, completions));
    }

    private CompletionStage<Void> openPublishedActor(
            ZLinkSpotRetireControl.StageRequest request, ActorTargetStage target) {
        return openAndNormalize(
                request,
                () -> {
                    openActorAdmission(request, target);
                    return CompletableFuture.completedFuture(null);
                });
    }

    private void openActorAdmission(
            ZLinkSpotRetireControl.StageRequest request, ActorTargetStage target) {
        actorStaging.openAdmission(target.staged());
        releasePublishedActor(request, target);
    }

    private CompletionStage<Void> openAndNormalize(
            ZLinkSpotRetireControl.StageRequest request,
            Supplier<CompletionStage<Void>> openDispatch) {
        return openDispatch
                .get()
                .thenCompose(
                        ignored ->
                                ZLinkSerialExecutionQueue.yieldCurrent(
                                        normalizer.normalize(request)));
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
            ZLinkUserSpotAggregateStagingOwner.Request decoded,
            TargetIngressAuthority authority) {
        return staging.stage(decoded, NOT_CANCELLED)
                .thenCompose(
                        value -> {
                            TargetStage target = new TargetStage(request, value, authority);
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
            TargetIngressAuthority authority,
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
                                            authority,
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
                            SpotDispatchLine context =
                                    Objects.requireNonNull(
                                            SpotDispatchLine.from(directAdmission.targetSpot()));
                            context.enqueueLifecycle(
                                            () ->
                                                    ZLinkSerialExecutionQueue.yieldCurrent(
                                                                    target.publishRequested())
                                                            .thenCompose(
                                                                    completions ->
                                                                            publishDirectJoinActor(
                                                                                    request,
                                                                                    target,
                                                                                    completions)))
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
            ZLinkUserSpotAggregateStagingOwner.Staged staged,
            TargetIngressAuthority authority) {}

    private record ActorTargetStage(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkStandaloneActorRelocationStagingOwner.Request targetRequest,
            ZLinkStandaloneActorRelocationStagingOwner.Staged staged,
            TargetIngressAuthority authority,
            ZLinkActorJoinRelocationPort.Admission directAdmission,
            ZLinkActorJoinCanonicalAdapter.PreviousMembership previousMembership,
            CompletableFuture<ZLinkTargetRelocationReplies> publishRequested,
            CompletableFuture<Void> joinPublished) {}

    private record CanonicalReply(
            long operationHigh,
            long operationLow,
            String sourceOwnerId,
            long sourceOwnerLeaseGeneration,
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            long sequence,
            int terminalResult,
            int failureCode,
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
