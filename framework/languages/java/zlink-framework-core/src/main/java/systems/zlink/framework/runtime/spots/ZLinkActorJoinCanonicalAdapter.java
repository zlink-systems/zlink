package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.Arrays;
import java.util.Optional;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkActorJoinRecoveryCodec;

/** Direct-Join goal/profile adapter owned by the canonical relocation host. */
final class ZLinkActorJoinCanonicalAdapter
    implements ZLinkActorJoinRelocationPort {
    private static final Logger LOGGER = Logger.getLogger(
        ZLinkActorJoinCanonicalAdapter.class.getName());

    private final ZLinkActorRuntime actors;
    private final ZLinkSpotRuntime spots;
    private final ConcurrentMap<RoutingId, Lane> lanes =
        new ConcurrentHashMap<>();
    private final ConcurrentMap<UUID, Admission> admissions =
        new ConcurrentHashMap<>();
    private final ConcurrentMap<UUID, CanonicalAdmission> canonicalAdmissions =
        new ConcurrentHashMap<>();
    private final ConcurrentMap<String, SourceAttempt> sources =
        new ConcurrentHashMap<>();
    private final ZLinkActorJoinPrewarmRegistry prewarm =
        new ZLinkActorJoinPrewarmRegistry();

    ZLinkActorJoinCanonicalAdapter(
        ZLinkActorRuntime actors,
        ZLinkSpotRuntime spots) {
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
                "canonical Actor Join lane is already registered: "
                    + localNodeRid);
        }
        node.setActorLeftHandler(this::handleActorLeft);
    }

    @Override
    public CompletionStage<Submission> relocate(Goal goal, Duration timeout) {
        Objects.requireNonNull(goal, "goal");
        Objects.requireNonNull(timeout, "timeout");
        Lane lane = lanes.get(goal.sourceActor().nodeRid());
        if (lane == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "canonical Actor Join source lane is unavailable"));
        }
        long deadline = deadline(timeout);
        ZLinkStoreCancellation cancellation = () ->
            System.nanoTime() - deadline >= 0;
        AtomicReference<ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource>
            preparedForDeadline = new AtomicReference<>();
        CompletionStage<Submission> operation =
            lane.source().prepareDirectJoin(goal, cancellation)
                .thenCompose(prepared -> {
                    preparedForDeadline.set(prepared);
                    return executeSource(lane, goal, timeout, prepared);
                });
        //  Spec 15 §3/§4 — the Join computes one absolute deadline at
        //  Defer() and every asynchronously completed Join must reach a
        //  completion callback; a location change not committed by the
        //  deadline is DeadlineExceeded (15-spot-actor:364). The cooperative
        //  cancellation predicate alone cannot end waits that never re-check
        //  it (store reads, lost one-way controls), so race the whole source
        //  operation against the same absolute deadline instead of letting
        //  the deferred Join hang silently.
        CompletableFuture<Submission> bounded = new CompletableFuture<>();
        operation.whenComplete((submission, failure) -> {
            if (failure == null) {
                bounded.complete(submission);
            } else {
                bounded.completeExceptionally(unwrap(failure));
            }
        });
        CompletableFuture.delayedExecutor(
                Math.max(1L, timeout.toMillis()), TimeUnit.MILLISECONDS)
            .execute(() -> {
                if (bounded.isDone()) {
                    return;
                }
                //  The prepared source sealed the actor's queue; without an
                //  abort the sealed queue also blocks the Failed completion
                //  delivery, so the deadline would fire and still leave the
                //  Join silent (spec 15 — every asynchronously completed
                //  Join reaches a completion callback).
                ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource
                    prepared = preparedForDeadline.get();
                if (prepared != null) {
                    prepared.abort().exceptionally(ignored -> null);
                }
                bounded.completeExceptionally(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        "Actor Join source relocation missed its deadline"));
            });
        return bounded;
    }

    private CompletionStage<Submission> executeSource(
        Lane lane,
        Goal goal,
        Duration timeout,
        ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource prepared) {
        SourceAttempt attempt = new SourceAttempt(goal, prepared);
        SourceAttempt previous = sources.putIfAbsent(
            goal.sourceActor().actorId(), attempt);
        if (previous != null) {
            return prepared.abort().thenCompose(ignored ->
                CompletableFuture.failedFuture(new IllegalStateException(
                    "canonical Actor Join source is already active")));
        }
        return lane.client().stage(
                goal.targetNodeRid(), prepared.stageRequest(), timeout)
            .thenCompose(ignored -> prepared.relayCapturedIngress(
                lane.client(), timeout))
            .thenCompose(ignored -> lane.client().publish(
                    goal.targetNodeRid(),
                    prepared.stageRequest().fence(),
                    timeout)
                .exceptionally(failure -> {
                    LOGGER.warning("Actor Join CUTOVER submission failed; "
                        + "the armed target fallback remains authoritative: "
                        + unwrap(failure));
                    return null;
                }))
            .thenApply(ignored -> {
                prepared.completeSourceQueueCommit();
                attempt.committed().complete(null);
                armAuthorityLossCleanup(attempt);
                return new Submission(new ZLinkBackendActorRef(
                    goal.targetNodeRid(),
                    goal.sourceActor().actorId(),
                    goal.sourceActor().generation()));
            })
            .exceptionallyCompose(failure -> {
                sources.remove(goal.sourceActor().actorId(), attempt);
                attempt.committed().completeExceptionally(unwrap(failure));
                return prepared.abort().handle((ignored, abortFailure) -> {
                    Throwable cause = unwrap(failure);
                    if (abortFailure != null) {
                        cause.addSuppressed(unwrap(abortFailure));
                    }
                    throw new CompletionException(cause);
                });
            });
    }

    private void armAuthorityLossCleanup(SourceAttempt attempt) {
        spots.scheduleRelocationCleanup(
            Instant.now().plus(spots.relocationForwardRetention()),
            () -> {
                sources.remove(
                    attempt.goal().sourceActor().actorId(), attempt);
                attempt.prepared().committedTargetAuthorityOwnerGeneration()
                    .thenCompose(generation -> generation.isPresent()
                            && attempt.leaveClaimed().compareAndSet(false, true)
                        ? completeSourceLeave(
                            attempt,
                            actorLeft(attempt, generation.orElseThrow()))
                        : CompletableFuture.completedFuture(null))
                    .exceptionally(failure -> {
                        LOGGER.warning(
                            "Actor Join authority-loss cleanup failed: "
                                + unwrap(failure));
                        return null;
                    });
            });
    }

    @Override
    public void admit(Admission admission) {
        Objects.requireNonNull(admission, "admission");
        Admission previous = admissions.putIfAbsent(
            admission.relocationId(), admission);
        if (previous != null
            && (!previous.operationId().equals(admission.operationId())
                || !previous.actorId().equals(admission.actorId())
                || !previous.targetSpotId().equals(
                    admission.targetSpotId()))) {
            throw new IllegalStateException(
                "canonical Actor Join admission fence conflicts");
        }
        if (previous == null) {
            //  Register the relocation temporary queue and validate the
            //  factory for this object before Accepted returns to the
            //  source (spec 15 §4.2). A different RelocationId already
            //  attempted the same object is evicted first — newest
            //  attempt wins (spec 15 §4.2 "같은 object의 relocation
            //  temporary queue는 하나만 존재한다").
            prewarm.register(
                admission.relocationId(),
                admission.actorId(),
                admission.objectGeneration(),
                admission.actorType(),
                actors.resolveActorFactoryType(admission.actorType()),
                evicted -> {
                    admissions.remove(evicted.relocationId());
                    LOGGER.info("Actor Join prewarm for "
                        + evicted.objectKey().actorId()
                        + " evicted by a newer RelocationId: "
                        + admission.relocationId());
                });
            //  Same validity-window timer covers both the admission
            //  record and its prewarm so an accepted-but-never-started
            //  move (DisableRelocation, capacity, compat failure) cleans
            //  both up together (spec 15 §4.2).
            spots.scheduleRelocationCleanup(
                Instant.now().plus(admission.timeout()),
                () -> {
                    admissions.remove(
                        admission.relocationId(), admission);
                    prewarm.release(admission.relocationId());
                });
        }
    }

    @Override
    public void admitCanonical(CanonicalAdmission admission) {
        Objects.requireNonNull(admission, "admission");
        CanonicalAdmission previous = canonicalAdmissions.putIfAbsent(
            admission.relocationId(), admission);
        if (previous != null) {
            if (!sameCanonicalAdmission(previous, admission)) {
                throw new IllegalStateException(
                    "canonical Actor Join recovery admission fence conflicts");
            }
            return;
        }
        prewarm.register(
            admission.relocationId(),
            admission.actorId(),
            admission.objectGeneration(),
            admission.actorType(),
            actors.resolveActorFactoryType(admission.actorType()),
            evicted -> {
                canonicalAdmissions.remove(evicted.relocationId());
                admissions.remove(evicted.relocationId());
            });
        spots.scheduleRelocationCleanup(
            Instant.now().plus(admission.timeout()),
            () -> {
                canonicalAdmissions.remove(
                    admission.relocationId(), admission);
                admissions.remove(admission.relocationId());
                prewarm.release(admission.relocationId());
            });
    }

    /**
     * Claims the approval-only command-28 admission with the durable ZLJR
     * identity carried by command 40. Returns false only for ordinary Actor
     * relocation, where neither side of that identity exists.
     */
    boolean claimRecovery(ZLinkSpotRetireControl.StageRequest request) {
        UUID relocationId = request.fence().aggregateId();
        CanonicalAdmission candidate = canonicalAdmissions.get(relocationId);
        Optional<ZLinkActorJoinRecoveryCodec.Recovery> decoded;
        try {
            decoded = ZLinkActorJoinRecoveryCodec.decodeFromEnvelope(
                request.relocationPayload());
        } catch (RuntimeException malformed) {
            throw recoveryRejected(
                "canonical Actor Join recovery payload is malformed", malformed);
        }
        if (decoded.isEmpty()) {
            if (candidate != null) {
                throw recoveryRejected(
                    "canonical Actor Join relocation is missing ZLJR saved work",
                    null);
            }
            return false;
        }
        if (candidate == null) {
            throw recoveryRejected(
                "canonical Actor Join recovery has no matching command-28 admission",
                null);
        }
        ZLinkActorJoinRecoveryCodec.Recovery recovery = decoded.orElseThrow();
        var participant = request.participants().getFirst();
        byte[] expectedReply = candidate.reply().toEncodedPayload(
            spots.serializerForSpot()).bytes();
        requireRecoveryFence(
            recovery.relocationId().equals(relocationId), "handoff id");
        requireRecoveryFence(
            recovery.actorId().equals(candidate.actorId()), "actor id");
        requireRecoveryFence(
            recovery.actorType().equals(candidate.actorType()), "actor type");
        requireRecoveryFence(
            recovery.actorGeneration() == candidate.objectGeneration(),
            "actor generation");
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
            recovery.expectedOwnerLeaseGeneration()
                == candidate.sourceOwnerLeaseGeneration(),
            "source owner lease generation");
        requireRecoveryFence(
            recovery.targetSpotId().equals(candidate.targetSpotId()),
            "target Spot id");
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
            Arrays.equals(recovery.request(), candidate.rawRequest()),
            "request body");
        requireRecoveryFence(
            "application/x-zlink-multipart".equals(
                recovery.replyContentType()),
            "reply content type");
        requireRecoveryFence(
            Arrays.equals(recovery.reply(), expectedReply), "reply body");
        requireRecoveryFence(
            recovery.sourceNodeRid().equals(request.sourceNodeRid()),
            "stage source node routing id");
        requireRecoveryFence(
            recovery.actorNodeGeneration() == request.sourceNodeGeneration(),
            "stage source node generation");
        requireRecoveryFence(
            recovery.expectedOwnerLeaseGeneration()
                == request.sourceOwnerLeaseGeneration(),
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
            candidate.targetOwnerLeaseGeneration()
                == request.targetOwnerLeaseGeneration(),
            "stage target owner lease generation");
        requireRecoveryFence(
            participant.objectId().equals(recovery.actorId()),
            "participant actor id");
        requireRecoveryFence(
            participant.objectGeneration() == recovery.actorGeneration(),
            "participant actor generation");
        requireRecoveryFence(
            participant.sourceAuthorityOwnerGeneration()
                == recovery.actorAuthorityOwnerGeneration(),
            "participant authority owner generation");
        requireRecoveryFence(
            recovery.targetAuthorityOwnerGeneration()
                == Math.addExact(
                    participant.sourceAuthorityOwnerGeneration(), 1L),
            "target authority owner generation");
        ZLinkMessage reply = recovery.reply().length == 0
            ? ZLinkMessage.empty()
            : ZLinkMessage.fromEncoded(
                ZLinkEncodedPayload.from(recovery.reply()),
                spots.serializerForSpot());
        Admission claimed = new Admission(
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
            throw recoveryRejected(
                "canonical Actor Join recovery claim conflicts", null);
        }
        return true;
    }

    /**
     * Looks up the attempt registered at admission time so PREPARE
     * (Restore) can verify the object identity
     * (ActorId + ObjectGeneration) before installing the real stage.
     */
    Optional<ZLinkActorJoinPrewarmRegistry.Attempt> findPrewarm(
        UUID relocationId) {
        return prewarm.find(relocationId);
    }

    /**
     * Production ingress lookup (spec 15 §4.2): delivers straight into
     * the real stage if PREPARE already installed one for this object,
     * parks the arrival if only the admission-time placeholder exists,
     * or reports not-found so the caller can fall through to other
     * ingress handling.
     */
    ZLinkActorJoinPrewarmRegistry.IngressRoute routeIngress(
        String actorId,
        long objectGeneration,
        ZLinkActorJoinPrewarmRegistry.ParkedMessage message) {
        return prewarm.parkOrDeliver(actorId, objectGeneration, message);
    }

    /**
     * Installs the real stage PREPARE produced and atomically migrates
     * every arrival parked since admission into it, in order (spec 15
     * §4.2). {@code liveAbort} tears the installed stage down if a newer
     * exact identity for the same object evicts this attempt before
     * publish commits.
     *
     * @throws IllegalStateException if this identity was evicted before
     *     PREPARE reached this call — the late PREPARE for a dead
     *     identity must be discarded, not installed.
     */
    void completeMigration(
        UUID relocationId,
        Consumer<ZLinkActorJoinPrewarmRegistry.ParkedMessage> deliver,
        Runnable installed,
        Runnable liveAbort) {
        prewarm.completeMigration(relocationId, deliver, installed, liveAbort);
    }

    /**
     * Releases the attempt reserved at admission time. Callers use this
     * once it has been consumed into the real staged Actor (normal
     * PREPARE handling), when PREPARE is aborted, or for any other
     * cleanup of the admission-time reservation.
     */
    void releasePrewarm(UUID relocationId) {
        prewarm.release(relocationId);
    }

    Optional<Admission> findAdmission(
        ZLinkSpotRetireControl.StageRequest request) {
        Admission admission = admissions.get(
            request.fence().aggregateId());
        if (admission == null) {
            return Optional.empty();
        }
        if (!admission.actorId().equals(
                request.participants().getFirst().objectId())
            || !admission.targetSpotId().equals(request.spotId())) {
            throw new IllegalStateException(
                "canonical Actor Join target admission fence conflicts");
        }
        return Optional.of(admission);
    }

    ZLinkSpotRetireControl.TargetProfile applyTargetProfile(
        ZLinkSpotRetireControl.StageRequest request,
        long defaultActorSpotGeneration) {
        Admission admission = admissions.get(
            request.fence().aggregateId());
        if (admission == null) {
            return new ZLinkSpotRetireControl.TargetProfile(
                request, defaultActorSpotGeneration, 1);
        }
        if (!admission.actorId().equals(
                request.participants().getFirst().objectId())) {
            throw new IllegalStateException(
                "canonical Actor Join target admission fence conflicts");
        }
        var profiled = new ZLinkSpotRetireControl.StageRequest(
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
        long targetSpotGeneration = ((systems.zlink.framework.spots.ZLinkSpot<?>)
            admission.targetSpot()).context().objectGeneration();
        return new ZLinkSpotRetireControl.TargetProfile(
            profiled, targetSpotGeneration, 2);
    }

    CompletionStage<Void> notifyTargetJoined(
        Admission admission,
        ZLinkStandaloneActorRelocationStagingOwner.Staged staged) {
        ZLinkActor actor = prepared(staged).actor();
        actors.markRelocatedActorJoined(
            actor,
            prepared(staged).actorRef(),
            admission.targetSpotId(),
            (systems.zlink.framework.spots.ZLinkSpot<?>)
                admission.targetSpot());
        try {
            return Objects.requireNonNull(
                admission.joined().apply(actor),
                "target OnJoined callback returned null");
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
        var left = new ZLinkServiceM6BWireCodec.ActorLeft(
            new ZLinkServiceM6BWireCodec.ActorIdentity(
                request.participants().getFirst().objectId(),
                request.participants().getFirst().objectGeneration()),
            previous.spotId(),
            previous.spotGeneration(),
            targetAuthorityOwnerGeneration);
        try {
            return lane.node().sendActorLeft(request.sourceNodeRid(), left)
                .handle((ignored, failure) -> {
                    if (failure != null) {
                        LOGGER.warning("Actor Join source leave submission failed: "
                            + unwrap(failure));
                    }
                    return null;
                });
        } catch (RuntimeException failure) {
            LOGGER.warning("Actor Join source leave submission failed: "
                + failure);
            return CompletableFuture.completedFuture(null);
        }
    }

    CompletionStage<Void> notifyTargetAccepted(
        Admission admission,
        ZLinkStandaloneActorRelocationStagingOwner.Staged staged) {
        ZLinkActorRuntime.PreparedTransferredActor prepared = prepared(staged);
        ZLinkActor actor = prepared.actor();
        ActorRef actorRef = new ActorRef(
            prepared.actorRef().actorId(),
            prepared.actorRef().generation(),
            actor.context().meshName(),
            prepared.actorRef().nodeRid());
        return actors.invokeActorLifecycle(actor, () -> actor.onJoinCompleted(
            new ZLinkActorJoinCompletion.Accepted(
                admission.operationId(), actorRef, admission.reply())));
    }

    void completeTarget(Admission admission) {
        admissions.remove(admission.relocationId(), admission);
        canonicalAdmissions.remove(admission.relocationId());
        prewarm.release(admission.relocationId());
    }

    private static boolean sameCanonicalAdmission(
        CanonicalAdmission left,
        CanonicalAdmission right) {
        return left.relocationId().equals(right.relocationId())
            && left.actorId().equals(right.actorId())
            && left.actorType().equals(right.actorType())
            && left.objectGeneration() == right.objectGeneration()
            && left.sourceNodeRid().equals(right.sourceNodeRid())
            && left.sourceNodeGeneration() == right.sourceNodeGeneration()
            && left.sourceAuthorityOwnerGeneration()
                == right.sourceAuthorityOwnerGeneration()
            && left.sourceOwnerLeaseGeneration()
                == right.sourceOwnerLeaseGeneration()
            && left.targetSpotId().equals(right.targetSpotId())
            && left.targetSpotGeneration() == right.targetSpotGeneration()
            && left.targetNodeRid().equals(right.targetNodeRid())
            && left.targetNodeGeneration() == right.targetNodeGeneration()
            && left.targetSpotAuthorityOwnerGeneration()
                == right.targetSpotAuthorityOwnerGeneration()
            && left.targetOwnerLeaseGeneration()
                == right.targetOwnerLeaseGeneration()
            && left.requestContentType().equals(right.requestContentType())
            && Arrays.equals(left.rawRequest(), right.rawRequest());
    }

    private static ZLinkFrameworkException recoveryRejected(
        String message,
        Throwable failure) {
        return failure == null
            ? new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DATA_LOST, message)
            : new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DATA_LOST, message, failure);
    }

    private static void requireRecoveryFence(
        boolean condition,
        String field) {
        if (!condition) {
            throw recoveryRejected(
                "canonical Actor Join recovery " + field + " conflicts",
                null);
        }
    }

    private CompletionStage<Void> handleActorLeft(
        RoutingId transportSource,
        ZLinkServiceM6BWireCodec.ActorLeft left) {
        SourceAttempt attempt = sources.get(left.actor().actorId());
        if (attempt == null
            || !transportSource.equals(attempt.goal().targetNodeRid())
            || left.actor().generation()
                != attempt.goal().sourceActor().generation()
            || !left.previousSpotId().equals(
                attempt.prepared().sourceSpotId())
            || left.previousSpotGeneration()
                != attempt.prepared().sourceSpotGeneration()
            || left.currentAuthorityOwnerGeneration()
                <= attempt.prepared().sourceAuthorityOwnerGeneration()) {
            return CompletableFuture.completedFuture(null);
        }
        return attempt.committed().thenCompose(ignored -> {
            if (!attempt.leaveClaimed().compareAndSet(false, true)) {
                return CompletableFuture.completedFuture(null);
            }
            attempt.prepared().acceptTargetAuthorityOwnerGeneration(
                left.currentAuthorityOwnerGeneration());
            return completeSourceLeave(attempt, left);
        });
    }

    private CompletionStage<Void> completeSourceLeave(
        SourceAttempt attempt,
        ZLinkServiceM6BWireCodec.ActorLeft left) {
        attempt.leaveClaimed().set(true);
        ZLinkActor actor = attempt.prepared().actor();
        CompletionStage<Void> lifecycle;
        if (actor == null) {
            lifecycle = CompletableFuture.completedFuture(null);
        } else {
            lifecycle = spots.notifySourceActorLeftForRemoteMove(actor)
                .exceptionally(failure -> {
                    LOGGER.warning("Actor Join source OnLeave failed: "
                        + unwrap(failure));
                    return null;
                });
        }
        return lifecycle
            .thenCompose(ignored -> attempt.prepared().cleanupLocal())
            .thenCompose(ignored ->
                attempt.prepared().discardInitialAfterCommit())
            .whenComplete((ignored, failure) ->
                sources.remove(left.actor().actorId(), attempt));
    }

    private static ZLinkServiceM6BWireCodec.ActorLeft actorLeft(
        SourceAttempt attempt,
        long targetAuthorityOwnerGeneration) {
        return new ZLinkServiceM6BWireCodec.ActorLeft(
            new ZLinkServiceM6BWireCodec.ActorIdentity(
                attempt.goal().sourceActor().actorId(),
                attempt.goal().sourceActor().generation()),
            attempt.prepared().sourceSpotId(),
            attempt.prepared().sourceSpotGeneration(),
            targetAuthorityOwnerGeneration);
    }

    private static ZLinkActorRuntime.PreparedTransferredActor prepared(
        ZLinkStandaloneActorRelocationStagingOwner.Staged staged) {
        return (ZLinkActorRuntime.PreparedTransferredActor) staged.actor();
    }

    private static long deadline(Duration timeout) {
        long nanos = timeout.toNanos();
        long now = System.nanoTime();
        return nanos > 0 && now > Long.MAX_VALUE - nanos
            ? Long.MAX_VALUE
            : now + nanos;
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    record PreviousMembership(String spotId, long spotGeneration) {
        PreviousMembership {
            if (spotId == null || spotId.isBlank() || spotGeneration <= 0) {
                throw new IllegalArgumentException(
                    "previous Actor membership is invalid");
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

    private record SourceAttempt(
        Goal goal,
        ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource prepared,
        CompletableFuture<Void> committed,
        AtomicBoolean leaveClaimed) {
        private SourceAttempt(
            Goal goal,
            ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource prepared) {
            this(
                goal,
                prepared,
                new CompletableFuture<>(),
                new AtomicBoolean());
        }
    }
}
