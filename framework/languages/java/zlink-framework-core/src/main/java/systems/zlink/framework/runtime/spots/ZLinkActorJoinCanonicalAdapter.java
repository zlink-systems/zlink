package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.internal.service.ZLinkActorJoinRecoveryCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.time.Duration;
import java.time.Instant;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Direct-Join goal/profile adapter owned by the canonical relocation host. */
final class ZLinkActorJoinCanonicalAdapter implements ZLinkActorJoinRelocationPort {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkActorJoinCanonicalAdapter.class.getName());

    private final ZLinkActorRuntime actors;
    private final ZLinkSpotRuntime spots;
    private final ConcurrentMap<RoutingId, Lane> lanes = new ConcurrentHashMap<>();
    private final ConcurrentMap<UUID, Admission> admissions = new ConcurrentHashMap<>();
    private final ConcurrentMap<UUID, CanonicalAdmission> canonicalAdmissions =
            new ConcurrentHashMap<>();
    private final ConcurrentMap<String, SourceAttempt> sources = new ConcurrentHashMap<>();
    private final ZLinkActorJoinPrewarmRegistry prewarm =
            new ZLinkActorJoinPrewarmRegistry(
                    relocationId -> {
                        canonicalAdmissions.remove(relocationId);
                        admissions.remove(relocationId);
                    });

    ZLinkActorJoinCanonicalAdapter(ZLinkActorRuntime actors, ZLinkSpotRuntime spots) {
        this.actors = Objects.requireNonNull(actors, "actors");
        this.spots = Objects.requireNonNull(spots, "spots");
    }

    void register(
            RoutingId localNodeRid,
            ZLinkInternalMeshNode node,
            ZLinkStandaloneActorRelocationSourceBuilder source,
            ZLinkRelocationTransitionClient client) {
        Lane lane = new Lane(node, source, client);
        if (lanes.putIfAbsent(localNodeRid, lane) != null) {
            throw new IllegalStateException(
                    "canonical Actor Join lane is already registered: " + localNodeRid);
        }
        node.setActorLeftHandler(this::handleActorLeft);
    }

    @Override
    public CompletionStage<Submission> relocate(Goal goal, Duration timeout) {
        Objects.requireNonNull(goal, "goal");
        Objects.requireNonNull(timeout, "timeout");
        Lane lane = lanes.get(goal.sourceActor().nodeRid());
        if (lane == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("canonical Actor Join source lane is unavailable"));
        }
        long deadline = deadline(timeout);
        ZLinkStoreCancellation cancellation = () -> System.nanoTime() - deadline >= 0;
        //  The Join deadline is also the Restore absolute deadline of its unit. Once relay
        //  readiness is accepted only authority settlement ends the Join (spec 28 §4.4).
        Instant restoreDeadline = Instant.now().plus(timeout);
        return lane.source()
                .prepareDirectJoin(goal, cancellation)
                .thenCompose(
                        prepared -> executeSource(lane, goal, timeout, restoreDeadline, prepared));
    }

    private CompletionStage<Submission> executeSource(
            Lane lane,
            Goal goal,
            Duration timeout,
            Instant restoreDeadline,
            ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource prepared) {
        SourceAttempt attempt = new SourceAttempt(goal, prepared);
        SourceAttempt claimed =
                sources.compute(
                        goal.sourceActor().actorId(),
                        (actorId, current) ->
                                current == null || current.committed().isDone()
                                        ? attempt
                                        : current);
        if (claimed != attempt) {
            return prepared.abort()
                    .whenComplete((ignored, failure) -> prepared.metrics.complete(null, false))
                    .thenCompose(
                            ignored ->
                                    CompletableFuture.failedFuture(
                                            new IllegalStateException(
                                                    "canonical Actor Join source is already"
                                                            + " active")));
        }
        return ZLinkRelocationHandOff.runToReady(
                        lane.client(),
                        prepared.stageRequest(),
                        settlement ->
                                prepared.relayCapturedIngress(lane.client(), timeout, settlement),
                        timeout,
                        restoreDeadline,
                        prepared.metrics)
                .handle(
                        (settlement, failure) -> {
                            if (failure != null) return abortSource(attempt, unwrap(failure), null);
                            CompletionStage<Void> cleanup =
                                    settlement
                                            .thenCompose(outcome -> settleSource(attempt, outcome))
                                            .thenApply(ignored -> null);
                            return CompletableFuture.completedFuture(
                                    new Submission(
                                            new ZLinkBackendActorRef(
                                                    goal.targetNodeRid(),
                                                    goal.sourceActor().actorId(),
                                                    goal.sourceActor().generation()),
                                            cleanup));
                        })
                .thenCompose(outcome -> outcome);
    }

    @Override
    public CompletionStage<Void> awaitSourceCleanup(String actorId, long objectGeneration) {
        SourceAttempt source = sources.get(actorId);
        return source == null || source.goal().sourceActor().generation() != objectGeneration
                ? CompletableFuture.completedFuture(null)
                : source.committed().thenApply(ignored -> null);
    }

    /** Applies the settled authority of the Join unit (spec 28 §4.4). */
    private CompletionStage<Submission> settleSource(
            SourceAttempt attempt, ZLinkRelocationTransitionClient.Settlement settlement) {
        Goal goal = attempt.goal();
        ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource prepared = attempt.prepared();
        return switch (settlement) {
            case TARGET_COMMITTED -> {
                yield prepared.completeSourceQueueCommit()
                        .thenCompose(ignored -> leaveSourceMembership(attempt))
                        .thenApply(
                                leave -> {
                                    attempt.committed().complete(leave);
                                    armAuthorityLossCleanup(attempt);
                                    return new Submission(
                                            new ZLinkBackendActorRef(
                                                    goal.targetNodeRid(),
                                                    goal.sourceActor().actorId(),
                                                    goal.sourceActor().generation()));
                                });
            }
            //  The source Preserve fence won at the Join deadline: the Actor stays on
            //  the source and the location change missed its deadline (spec 15).
            case SOURCE_PRESERVED ->
                    abortSource(
                            attempt,
                            new ZLinkFrameworkException(
                                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                                    "Actor Join target commit was not confirmed before its"
                                            + " deadline; the source Preserve fence won"),
                            settlement);
            case SOURCE_LEASE_EXPIRED -> {
                sources.remove(goal.sourceActor().actorId(), attempt);
                var expired =
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                "Actor Join source owner lease expired or authority is Missing");
                attempt.committed().completeExceptionally(expired);
                yield prepared.discardAfterSourceLeaseExpiry()
                        .handle(
                                (ignored, discardFailure) -> {
                                    if (discardFailure != null) {
                                        expired.addSuppressed(unwrap(discardFailure));
                                    }
                                    throw new CompletionException(expired);
                                });
            }
        };
    }

    /**
     * The confirmed target commit ends the source membership (spec 30 §7, spec 15 §4.1): the
     * committed Actor leaves the source registry and inventory here, as host relocation does. Only
     * the one-way source OnLeaveActor still waits for ActorLeft or the retention cleanup, so it
     * keeps the Spot surface resolved while the Actor was a member.
     */
    private CompletionStage<SourceLeave> leaveSourceMembership(SourceAttempt attempt) {
        ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource prepared = attempt.prepared();
        ZLinkActor actor = prepared.actor();
        SourceLeave leave =
                new SourceLeave(actor, actor == null ? null : spots.localActorSpotSurface(actor));
        return ZLinkRelocationHandOff.completeSourceCleanup(
                        List.of(prepared::cleanupLocal, prepared::discardInitialAfterCommit))
                .thenApply(ignored -> leave);
    }

    /** Restores the source: a failure before relay readiness or a won Preserve fence. */
    private CompletionStage<Submission> abortSource(
            SourceAttempt attempt,
            Throwable cause,
            ZLinkRelocationTransitionClient.Settlement settlement) {
        sources.remove(attempt.goal().sourceActor().actorId(), attempt);
        attempt.committed().completeExceptionally(cause);
        return attempt.prepared()
                .abort()
                .handle(
                        (ignored, abortFailure) -> {
                            if (abortFailure != null) {
                                cause.addSuppressed(unwrap(abortFailure));
                            }
                            if (settlement != null)
                                attempt.prepared()
                                        .metrics
                                        .complete(abortFailure == null ? settlement : null, false);
                            throw new CompletionException(cause);
                        });
    }

    private void armAuthorityLossCleanup(SourceAttempt attempt) {
        spots.scheduleRelocationCleanup(
                Instant.now().plus(spots.relocationForwardRetention()),
                () -> {
                    sources.remove(attempt.goal().sourceActor().actorId(), attempt);
                    attempt.committed()
                            .thenCompose(leave -> notifySourceLeft(attempt, leave))
                            .exceptionally(
                                    failure -> {
                                        LOGGER.log(
                                                Level.WARNING,
                                                "Actor Join authority-loss cleanup failed:",
                                                unwrap(failure));
                                        return null;
                                    });
                });
    }

    @Override
    public void admit(Admission admission) {
        Objects.requireNonNull(admission, "admission");
        Admission previous = admissions.putIfAbsent(admission.relocationId(), admission);
        if (previous != null
                && (!previous.operationId().equals(admission.operationId())
                        || !previous.actorId().equals(admission.actorId())
                        || !previous.targetSpotId().equals(admission.targetSpotId()))) {
            throw new IllegalStateException("canonical Actor Join admission fence conflicts");
        }
        if (previous == null) {
            //  Register the relocation temporary queue and validate the
            //  factory for this object before Accepted returns to the
            //  source (spec 15 §4.2). A different RelocationId already
            //  attempted the same object is evicted first — newest
            //  attempt wins (spec 15 §4.2 "같은 object의 relocation
            //  temporary queue는 하나만 존재한다").
            actors.resolveActorFactoryType(admission.actorType());
            prewarm.register(
                    admission.relocationId(),
                    admission.actorId(),
                    admission.objectGeneration(),
                    evicted -> {});
            spots.scheduleRelocationCleanup(
                    Instant.now().plus(admission.timeout()),
                    () -> prewarm.expire(admission.relocationId()));
        }
    }

    @Override
    public void admitCanonical(CanonicalAdmission admission) {
        Objects.requireNonNull(admission, "admission");
        CanonicalAdmission previous =
                canonicalAdmissions.putIfAbsent(admission.relocationId(), admission);
        if (previous != null) {
            if (!sameCanonicalAdmission(previous, admission)) {
                throw new IllegalStateException(
                        "canonical Actor Join recovery admission fence conflicts");
            }
            return;
        }
        actors.resolveActorFactoryType(admission.actorType());
        prewarm.register(
                admission.relocationId(),
                admission.actorId(),
                admission.objectGeneration(),
                evicted -> {});
        spots.scheduleRelocationCleanup(
                Instant.now().plus(admission.timeout()),
                () -> prewarm.expire(admission.relocationId()));
    }

    CompletionStage<Void> beginTarget(
            UUID relocationId, CompletionStage<Void> terminal, Runnable superseded) {
        return prewarm.begin(relocationId, terminal, superseded);
    }

    CompletionStage<Void> endTarget(UUID relocationId) {
        return prewarm.releaseAsync(relocationId);
    }

    /**
     * Claims the approval-only command-28 admission with the durable ZLJR identity carried by
     * command 40. Returns false only for ordinary Actor relocation, where neither side of that
     * identity exists.
     */
    boolean claimRecovery(ZLinkSpotRetireControl.StageRequest request) {
        UUID relocationId = request.fence().aggregateId();
        CanonicalAdmission candidate = canonicalAdmissions.get(relocationId);
        Optional<ZLinkActorJoinRecoveryCodec.Recovery> decoded;
        try {
            decoded = ZLinkActorJoinRecoveryCodec.decodeFromEnvelope(request.relocationPayload());
        } catch (RuntimeException malformed) {
            throw recoveryRejected("canonical Actor Join recovery payload is malformed", malformed);
        }
        if (decoded.isEmpty()) {
            if (candidate != null) {
                throw recoveryRejected(
                        "canonical Actor Join relocation is missing ZLJR saved work", null);
            }
            return false;
        }
        ZLinkActorJoinRecoveryCodec.Recovery recovery = decoded.orElseThrow();
        if (candidate == null) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "canonical Actor Join preparation admission is no longer available");
        }
        var participant = request.participants().getFirst();
        requireRecoveryFence(recovery.relocationId().equals(relocationId), "handoff id");
        requireRecoveryFence(recovery.actorId().equals(candidate.actorId()), "actor id");
        requireRecoveryFence(recovery.actorType().equals(candidate.actorType()), "actor type");
        if (recovery.actorGeneration() != candidate.objectGeneration()) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "canonical Actor Join recovery actor generation conflicts");
        }
        requireRecoveryFence(
                recovery.sourceNodeRid().equals(candidate.sourceNodeRid()),
                "source node routing id");
        requireRecoveryFence(
                recovery.actorNodeGeneration() == candidate.sourceNodeGeneration(),
                "source node generation");
        requireRecoveryFence(
                recovery.actorAuthorityOwnerGeneration()
                        == candidate.sourceAuthorityOwnerGeneration(),
                "source authority owner generation");
        requireRecoveryFence(
                recovery.expectedOwnerLeaseGeneration() == candidate.sourceOwnerLeaseGeneration(),
                "source owner lease generation");
        requireRecoveryFence(
                recovery.targetSpotId().equals(candidate.targetSpotId()), "target Spot id");
        requireRecoveryFence(
                recovery.targetSpotGeneration() == candidate.targetSpotGeneration(),
                "target Spot generation");
        requireRecoveryFence(
                recovery.targetNodeRid().equals(candidate.targetNodeRid()),
                "target node routing id");
        requireRecoveryFence(
                recovery.targetNodeGeneration() == candidate.targetNodeGeneration(),
                "target node generation");
        requireRecoveryFence(
                recovery.targetSpotAuthorityOwnerGeneration()
                        == candidate.targetSpotAuthorityOwnerGeneration(),
                "target Spot authority owner generation");
        requireRecoveryFence(
                recovery.requestContentType().equals(candidate.requestContentType()),
                "request content type");
        requireRecoveryFence(
                Arrays.equals(recovery.request(), candidate.rawRequest()), "request body");
        requireRecoveryFence(
                recovery.sourceNodeRid().equals(request.sourceNodeRid()),
                "stage source node routing id");
        requireRecoveryFence(
                recovery.actorNodeGeneration() == request.sourceNodeGeneration(),
                "stage source node generation");
        requireRecoveryFence(
                recovery.expectedOwnerLeaseGeneration() == request.sourceOwnerLeaseGeneration(),
                "stage source owner lease generation");
        requireRecoveryFence(
                recovery.coordinator().ownerId().equals(request.sourceOwnerId()),
                "stage source owner id");
        requireRecoveryFence(
                recovery.targetNodeRid().equals(request.targetNodeRid()),
                "stage target node routing id");
        requireRecoveryFence(
                recovery.targetNodeGeneration() == request.targetNodeGeneration(),
                "stage target node generation");
        requireRecoveryFence(
                candidate.targetOwnerLeaseGeneration() == request.targetOwnerLeaseGeneration(),
                "stage target owner lease generation");
        requireRecoveryFence(
                participant.objectId().equals(recovery.actorId()), "participant actor id");
        requireRecoveryFence(
                participant.objectGeneration() == recovery.actorGeneration(),
                "participant actor generation");
        requireRecoveryFence(
                participant.sourceAuthorityOwnerGeneration()
                        == recovery.actorAuthorityOwnerGeneration(),
                "participant authority owner generation");
        requireRecoveryFence(
                recovery.targetAuthorityOwnerGeneration()
                        == participant.sourceAuthorityOwnerGeneration(),
                "target authority owner generation");
        ZLinkMessage reply =
                recovery.reply().length == 0
                        ? ZLinkMessage.empty()
                        : ZLinkMessage.fromEncoded(
                                ZLinkEncodedPayload.from(recovery.reply()),
                                spots.serializerForSpot());
        Admission claimed =
                new Admission(
                        relocationId,
                        recovery.operationId(),
                        candidate.actorId(),
                        candidate.actorType(),
                        candidate.objectGeneration(),
                        candidate.targetSpotId(),
                        candidate.targetSpot(),
                        candidate.joined(),
                        reply,
                        candidate.timeout());
        Admission previous = admissions.putIfAbsent(relocationId, claimed);
        if (previous != null
                && (!previous.operationId().equals(claimed.operationId())
                        || !previous.actorId().equals(claimed.actorId())
                        || !previous.targetSpotId().equals(claimed.targetSpotId()))) {
            throw recoveryRejected("canonical Actor Join recovery claim conflicts", null);
        }
        return true;
    }

    /** Returns the reservation still owned by the canonical target attempt. */
    Optional<ZLinkActorJoinPrewarmRegistry.Attempt> findPrewarm(UUID relocationId) {
        return prewarm.find(relocationId);
    }

    /**
     * Production ingress lookup (spec 15 §4.2): delivers straight into the real stage if PREPARE
     * already installed one for this object, parks the arrival if only the admission-time
     * placeholder exists, or reports not-found so the caller can fall through to other ingress
     * handling.
     */
    ZLinkActorJoinPrewarmRegistry.IngressRoute routeIngress(
            String actorId,
            long objectGeneration,
            Supplier<ZLinkActorJoinPrewarmRegistry.ParkedMessage> message) {
        return prewarm.parkOrDeliver(actorId, objectGeneration, message);
    }

    /**
     * Installs the real stage PREPARE produced and atomically migrates every arrival parked since
     * admission into it, in order (spec 15 §4.2). {@code liveAbort} tears the installed stage down
     * if a newer exact identity for the same object evicts this attempt before publish commits.
     *
     * @throws IllegalStateException if this identity was evicted before PREPARE reached this call —
     *     the late PREPARE for a dead identity must be discarded, not installed.
     */
    void completeMigration(
            UUID relocationId,
            Consumer<ZLinkActorJoinPrewarmRegistry.ParkedMessage> deliver,
            Runnable installed,
            Runnable liveAbort) {
        prewarm.completeMigration(relocationId, deliver, installed, liveAbort);
    }

    /** Ends ingress forwarding when the stage closes; the target terminal releases admission. */
    void releasePrewarm(UUID relocationId) {
        prewarm.detachIngress(relocationId);
    }

    Optional<Admission> findAdmission(ZLinkSpotRetireControl.StageRequest request) {
        Admission admission = admissions.get(request.fence().aggregateId());
        if (admission == null) {
            return Optional.empty();
        }
        if (!admission.actorId().equals(request.participants().getFirst().objectId())
                || admission.objectGeneration()
                        != request.participants().getFirst().objectGeneration()
                || !admission.targetSpotId().equals(request.spotId())) {
            throw new IllegalStateException(
                    "canonical Actor Join target admission fence conflicts");
        }
        return Optional.of(admission);
    }

    ZLinkSpotRetireControl.TargetProfile applyTargetProfile(
            ZLinkSpotRetireControl.StageRequest request, long defaultActorSpotGeneration) {
        Admission admission = admissions.get(request.fence().aggregateId());
        if (admission == null) {
            return new ZLinkSpotRetireControl.TargetProfile(request, defaultActorSpotGeneration, 1);
        }
        if (!admission.actorId().equals(request.participants().getFirst().objectId())) {
            throw new IllegalStateException(
                    "canonical Actor Join target admission fence conflicts");
        }
        var profiled =
                new ZLinkSpotRetireControl.StageRequest(
                        request.fence(),
                        request.sourceNodeRid(),
                        request.sourceNodeGeneration(),
                        request.sourceOwnerId(),
                        request.sourceOwnerLeaseGeneration(),
                        request.targetNodeRid(),
                        request.targetNodeGeneration(),
                        request.targetOwnerId(),
                        request.targetOwnerLeaseGeneration(),
                        request.meshName(),
                        admission.targetSpotId(),
                        request.stableType(),
                        request.instanceSpot(),
                        request.restoreSpotSnapshot(),
                        request.relocationPayload(),
                        request.participants(),
                        request.sessionRoutes());
        long targetSpotGeneration =
                ((systems.zlink.framework.spots.ZLinkSpot<?>) admission.targetSpot())
                        .context()
                        .objectGeneration();
        return new ZLinkSpotRetireControl.TargetProfile(profiled, targetSpotGeneration, 2);
    }

    CompletionStage<Void> notifyTargetJoined(
            Admission admission, ZLinkStandaloneActorRelocationStagingOwner.Staged staged) {
        ZLinkActorRuntime.PreparedTransferredActor prepared = prepared(staged);
        ZLinkActor actor = prepared.actor();
        try {
            actors.markRelocatedActorJoined(
                    actor,
                    prepared.actorRef(),
                    admission.targetSpotId(),
                    (systems.zlink.framework.spots.ZLinkSpot<?>) admission.targetSpot());
            return Objects.requireNonNull(
                    admission.joined().apply(actor), "target OnJoined callback returned null");
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    CompletionStage<Void> submitSourceLeave(
            ZLinkSpotRetireControl.StageRequest request,
            PreviousMembership previous,
            long targetAuthorityOwnerGeneration) {
        Lane lane = lanes.get(request.targetNodeRid());
        if (lane == null) {
            LOGGER.warning("Actor Join source leave submission has no target lane");
            return CompletableFuture.completedFuture(null);
        }
        var left =
                new ZLinkServiceM6BWireCodec.ActorLeft(
                        new ZLinkServiceM6BWireCodec.ActorIdentity(
                                request.participants().getFirst().objectId(),
                                request.participants().getFirst().objectGeneration()),
                        previous.spotId(),
                        previous.spotGeneration(),
                        targetAuthorityOwnerGeneration);
        try {
            return lane.node()
                    .sendActorLeft(request.sourceNodeRid(), left)
                    .handle(
                            (ignored, failure) -> {
                                if (failure != null) {
                                    LOGGER.warning(
                                            "Actor Join source leave submission failed: "
                                                    + unwrap(failure));
                                }
                                return null;
                            });
        } catch (RuntimeException failure) {
            LOGGER.warning("Actor Join source leave submission failed: " + failure);
            return CompletableFuture.completedFuture(null);
        }
    }

    CompletionStage<Void> notifyTargetCompletion(
            Admission admission,
            ZLinkStandaloneActorRelocationStagingOwner.Staged staged,
            Throwable lifecycleFailure) {
        ZLinkActorRuntime.PreparedTransferredActor prepared = prepared(staged);
        ZLinkActor actor = prepared.actor();
        ZLinkActorJoinCompletion completion;
        if (lifecycleFailure == null) {
            ActorRef actorRef =
                    new ActorRef(
                            prepared.actorRef().actorId(),
                            prepared.actorRef().generation(),
                            actor.context().meshName(),
                            prepared.actorRef().nodeRid());
            completion =
                    new ZLinkActorJoinCompletion.Accepted(
                            admission.operationId(), actorRef, admission.reply());
        } else {
            Throwable cause = unwrap(lifecycleFailure);
            ZLinkFrameworkErrorKind kind =
                    cause instanceof ZLinkFrameworkException framework
                            ? framework.kind()
                            : ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
            completion = new ZLinkActorJoinCompletion.Failed(admission.operationId(), kind);
        }
        return actors.invokeActorLifecycle(actor, () -> actor.onJoinCompleted(completion));
    }

    private static boolean sameCanonicalAdmission(
            CanonicalAdmission left, CanonicalAdmission right) {
        return left.relocationId().equals(right.relocationId())
                && left.actorId().equals(right.actorId())
                && left.actorType().equals(right.actorType())
                && left.objectGeneration() == right.objectGeneration()
                && left.sourceNodeRid().equals(right.sourceNodeRid())
                && left.sourceNodeGeneration() == right.sourceNodeGeneration()
                && left.sourceAuthorityOwnerGeneration() == right.sourceAuthorityOwnerGeneration()
                && left.sourceOwnerLeaseGeneration() == right.sourceOwnerLeaseGeneration()
                && left.targetSpotId().equals(right.targetSpotId())
                && left.targetSpotGeneration() == right.targetSpotGeneration()
                && left.targetNodeRid().equals(right.targetNodeRid())
                && left.targetNodeGeneration() == right.targetNodeGeneration()
                && left.targetSpotAuthorityOwnerGeneration()
                        == right.targetSpotAuthorityOwnerGeneration()
                && left.targetOwnerLeaseGeneration() == right.targetOwnerLeaseGeneration()
                && left.requestContentType().equals(right.requestContentType())
                && Arrays.equals(left.rawRequest(), right.rawRequest());
    }

    private static ZLinkFrameworkException recoveryRejected(String message, Throwable failure) {
        return failure == null
                ? new ZLinkFrameworkException(ZLinkFrameworkErrorKind.DATA_LOST, message)
                : new ZLinkFrameworkException(ZLinkFrameworkErrorKind.DATA_LOST, message, failure);
    }

    private static void requireRecoveryFence(boolean condition, String field) {
        if (!condition) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "canonical Actor Join recovery " + field + " conflicts");
        }
    }

    private CompletionStage<Void> handleActorLeft(
            RoutingId transportSource, ZLinkServiceM6BWireCodec.ActorLeft left) {
        SourceAttempt attempt = sources.get(left.actor().actorId());
        if (attempt == null
                || !transportSource.equals(attempt.goal().targetNodeRid())
                || left.actor().generation() != attempt.goal().sourceActor().generation()
                || !left.previousSpotId().equals(attempt.prepared().sourceSpotId())
                || left.previousSpotGeneration() != attempt.prepared().sourceSpotGeneration()
                || left.currentAuthorityOwnerGeneration()
                        <= attempt.prepared().sourceAuthorityOwnerGeneration()) {
            return CompletableFuture.completedFuture(null);
        }
        return attempt.committed().thenCompose(leave -> notifySourceLeft(attempt, leave));
    }

    /** Delivers the one-way source OnLeaveActor; membership already ended at the commit. */
    private CompletionStage<Void> notifySourceLeft(SourceAttempt attempt, SourceLeave leave) {
        if (!attempt.leaveClaimed().compareAndSet(false, true)) {
            return CompletableFuture.completedFuture(null);
        }
        return ZLinkHandlerStages.fromStageSupplier(
                        () ->
                                leave.actor() == null
                                        ? CompletableFuture.completedFuture(null)
                                        : spots.notifySourceActorLeft(
                                                leave.spotSurface(), leave.actor()))
                .whenComplete(
                        (ignored, failure) ->
                                sources.remove(attempt.goal().sourceActor().actorId(), attempt));
    }

    private static ZLinkActorRuntime.PreparedTransferredActor prepared(
            ZLinkStandaloneActorRelocationStagingOwner.Staged staged) {
        return (ZLinkActorRuntime.PreparedTransferredActor) staged.actor();
    }

    private static long deadline(Duration timeout) {
        long nanos = timeout.toNanos();
        long now = System.nanoTime();
        return nanos > 0 && now > Long.MAX_VALUE - nanos ? Long.MAX_VALUE : now + nanos;
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    record PreviousMembership(String spotId, long spotGeneration) {
        PreviousMembership {
            if (spotId == null || spotId.isBlank() || spotGeneration <= 0) {
                throw new IllegalArgumentException("previous Actor membership is invalid");
            }
        }
    }

    private record Lane(
            ZLinkInternalMeshNode node,
            ZLinkStandaloneActorRelocationSourceBuilder source,
            ZLinkRelocationTransitionClient client) {
        private Lane {
            Objects.requireNonNull(node, "node");
            Objects.requireNonNull(source, "source");
            Objects.requireNonNull(client, "client");
        }
    }

    /** The source member that the committed Join left, kept for its one-way OnLeaveActor. */
    private record SourceLeave(ZLinkActor actor, Object spotSurface) {}

    private record SourceAttempt(
            Goal goal,
            ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource prepared,
            CompletableFuture<SourceLeave> committed,
            AtomicBoolean leaveClaimed) {
        private SourceAttempt(
                Goal goal, ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource prepared) {
            this(goal, prepared, new CompletableFuture<>(), new AtomicBoolean());
        }
    }
}
