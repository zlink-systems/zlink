package systems.zlink.framework.runtime.spots;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityEntry;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanCursor;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanExpired;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkCanonicalRelocationAuthorityStateCodec;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkServiceRelocationEnvelopeCodec;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkSpotTypeCapacityDelta;

/** Owns the exact prepare/ready/state/data/cutover relocation attempt state. */
final class ZLinkCanonicalRelocationStateMachine
    implements ZLinkCanonicalRelocationTransitionOwner.StateMachine,
        ZLinkRelocationTransitionClient {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final Logger LOGGER = Logger.getLogger(
        ZLinkCanonicalRelocationStateMachine.class.getName());
    private static final String ACTOR_AUTHORITY_PREFIX = "zla1:a:";
    private static final int SCAN_PAGE_SIZE = 1000;
    private static final Duration STORE_RETRY_DELAY = Duration.ofMillis(25);

    private final ZLinkInternalMeshNode node;
    private final String meshName;
    private final String entrySpotId;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkSpotRetireControl.TargetEndpoint target;
    private final RetentionScheduler retentionScheduler;
    private final ZLinkRelocationPayloadTransfer.Options transferOptions;
    private final ZLinkRelocationPayloadTransfer.Budget budget;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ConcurrentHashMap<Fence, SourceAttempt> sources =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Fence, TargetAttempt> targets =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Fence, TerminalTarget> terminalTargets =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Fence, RetainedSource> retainedSources =
        new ConcurrentHashMap<>();
    private final AtomicInteger openSourceQuiescenceWindows =
        new AtomicInteger();

    ZLinkCanonicalRelocationStateMachine(
        ZLinkInternalMeshNode node,
        String meshName,
        String entrySpotId,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkSpotRetireControl.TargetEndpoint target) {
        this(
            node,
            meshName,
            entrySpotId,
            locations,
            coordinator,
            target,
            ZLinkCanonicalRelocationStateMachine::scheduleAt);
    }

    ZLinkCanonicalRelocationStateMachine(
        ZLinkInternalMeshNode node,
        String meshName,
        String entrySpotId,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkSpotRetireControl.TargetEndpoint target,
        RetentionScheduler retentionScheduler) {
        this(
            node,
            meshName,
            entrySpotId,
            locations,
            coordinator,
            target,
            retentionScheduler,
            ZLinkRelocationPayloadTransfer.Options.defaults());
    }

    ZLinkCanonicalRelocationStateMachine(
        ZLinkInternalMeshNode node,
        String meshName,
        String entrySpotId,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkSpotRetireControl.TargetEndpoint target,
        RetentionScheduler retentionScheduler,
        ZLinkRelocationPayloadTransfer.Options transferOptions) {
        this.node = Objects.requireNonNull(node, "node");
        this.meshName = requireText(meshName, "meshName");
        this.entrySpotId = entrySpotId;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.target = Objects.requireNonNull(target, "target");
        this.retentionScheduler = Objects.requireNonNull(
            retentionScheduler, "retentionScheduler");
        this.transferOptions = Objects.requireNonNull(
            transferOptions, "transferOptions");
        this.budget = new ZLinkRelocationPayloadTransfer.Budget(
            transferOptions.chunkLimitBytes(),
            transferOptions.inFlightPayloadBudgetBytes(),
            transferOptions.nodeInFlightPayloadBudgetBytes());
        localNodeRid = node.status().routingId();
        localNodeGeneration = node.status().lifecycleGeneration();
    }

    /**
     * Waits until a new relocation unit may apply its source admission seal
     * — full in-flight budget delays the seal, never the running unit
     * (spec 28 §5.3).
     */
    CompletionStage<Void> awaitUnitAdmission() {
        return budget.awaitUnitAdmission();
    }

    /**
     * True when this source keeps no relocation payload or boundary batch
     * copy and no Message Follow route obligation — the SafeToShutdown
     * component this machine owns (spec 30 §11).
     */
    boolean sourceQuiescent() {
        return openSourceQuiescenceWindows.get() == 0;
    }

    @Override
    public CompletionStage<Void> stage(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.StageRequest request,
        Duration timeout) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        Objects.requireNonNull(request, "request");
        requireTimeout(timeout);
        if (!targetNodeRid.equals(request.targetNodeRid())) {
            return failed(new IllegalArgumentException(
                "canonical relocation target RID differs"));
        }
        return sourcePrepare(request).thenCompose(prepare -> {
            Fence fence = new Fence(
                prepare.id(), prepare.targetAttemptGeneration());
            SourceAttempt created = new SourceAttempt(request, prepare);
            SourceAttempt current = sources.putIfAbsent(fence, created);
            SourceAttempt attempt = current == null ? created : current;
            if (!attempt.request().equals(request)
                || !java.util.Arrays.equals(
                    ZLinkCanonicalRelocationProtocol.encodePrepare(
                        attempt.prepare()),
                    ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))) {
                return failed(new IllegalArgumentException(
                    "duplicate canonical relocation prepare differs"));
            }
            attempt.activeWaiters().incrementAndGet();
            long readyDeadlineNanos = System.nanoTime() + timeout.toNanos();
            return awaitReadyWithPrepareResend(
                    targetNodeRid,
                    attempt,
                    readyDeadlineNanos,
                    current == null || !attempt.ready().isDone())
                .whenComplete((ignored, failure) -> {
                    int remaining = attempt.activeWaiters().decrementAndGet();
                    if (failure != null
                        && remaining == 0) {
                        sources.remove(fence, attempt);
                    }
                });
        });
    }

    @Override
    public CompletionStage<Void> relay(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        byte[] frozenRecord,
        Duration timeout) {
        Objects.requireNonNull(frozenRecord, "frozenRecord");
        requireTimeout(timeout);
        SourceAttempt attempt = requireSource(Fence.from(fence), targetNodeRid);
        var prepare = attempt.prepare();
        byte[] encoded = ZLinkCanonicalRelocationProtocol.encodeData(
            new ZLinkCanonicalRelocationProtocol.Data(
                prepare.id(),
                prepare.targetAttemptGeneration(),
                prepare.coordinator(),
                ZLinkCanonicalRelocationProtocol.SOURCE,
                prepare.object(),
                frozenRecord));
        attempt.batch().append(frozenRecord, encoded);
        return send(targetNodeRid, encoded);
    }

    @Override
    public CompletionStage<Void> publish(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout) {
        requireTimeout(timeout);
        SourceAttempt attempt = requireSource(Fence.from(fence), targetNodeRid);
        var prepare = attempt.prepare();
        RelayBatch batch = attempt.batch();
        var cutover = new ZLinkCanonicalRelocationProtocol.Cutover(
            prepare.id(),
            prepare.targetAttemptGeneration(),
            prepare.coordinator(),
            ZLinkCanonicalRelocationProtocol.SOURCE,
            prepare.object(),
            batch.recordCount(),
            batch.checksumCrc32c());
        Fence key = Fence.from(fence);
        byte[] encodedCutover =
            ZLinkCanonicalRelocationProtocol.encodeCutover(cutover);
        return send(targetNodeRid, encodedCutover)
            .whenComplete((ignored, failure) -> {
                //  Cutover submit terminal (S1). The payload and boundary
                //  batch copies stay retained for the retransmission window
                //  regardless of the submit result (spec 28 §4.4).
                sources.remove(key, attempt);
                retainSourceCopies(key, attempt, encodedCutover);
            });
    }

    private void retainSourceCopies(
        Fence key,
        SourceAttempt attempt,
        byte[] encodedCutover) {
        RetainedSource retained = new RetainedSource(
            attempt.request().targetNodeRid(),
            attempt.batch().encodedFrames(),
            encodedCutover);
        if (retainedSources.putIfAbsent(key, retained) != null) {
            return;
        }
        long submitNanos = System.nanoTime();
        openSourceQuiescenceWindows.incrementAndGet();
        Instant now = Instant.now();
        //  Exactly-once copy cleanup after the retransmission window.
        retentionScheduler.schedule(
            now.plus(transferOptions.cutoverWaitTimeout()),
            () -> retainedSources.remove(key, retained));
        //  Source quiescence (SafeToShutdown component): the unit is done
        //  when both the retransmission window and the Message Follow route
        //  window (S4) have elapsed — both source-local (spec 30 §11).
        Duration quiescence = transferOptions.cutoverWaitTimeout()
            .compareTo(transferOptions.messageFollowDuration()) >= 0
            ? transferOptions.cutoverWaitTimeout()
            : transferOptions.messageFollowDuration();
        retentionScheduler.schedule(
            now.plus(quiescence),
            () -> {
                openSourceQuiescenceWindows.decrementAndGet();
                ZLinkRuntimeMetrics.record(
                    "zlink.relocation.route_convergence",
                    Duration.ofNanos(System.nanoTime() - submitNanos),
                    Map.of());
            });
    }

    /**
     * Resends the boundary batch and cutover on the current connection while
     * the retransmission window is open. The target replaces its partially
     * received pre-boundary section with the whole batch (spec 28 §4.4).
     */
    CompletionStage<Void> retransmitBoundaryBatch(
        ZLinkSpotRetireControl.Fence fence) {
        RetainedSource retained = retainedSources.get(Fence.from(fence));
        if (retained == null) {
            return CompletableFuture.completedFuture(null);
        }
        CompletionStage<Void> chain =
            CompletableFuture.completedFuture(null);
        for (byte[] frame : retained.dataFrames()) {
            chain = chain.thenCompose(
                ignored -> send(retained.targetNodeRid(), frame));
        }
        return chain.thenCompose(ignored ->
            send(retained.targetNodeRid(), retained.encodedCutover()));
    }

    @Override
    public CompletionStage<Void> abort(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout) {
        requireTimeout(timeout);
        Fence key = Fence.from(fence);
        SourceAttempt attempt = sources.get(key);
        if (attempt == null) {
            return CompletableFuture.completedFuture(null);
        }
        if (!attempt.request().targetNodeRid().equals(targetNodeRid)) {
            return failed(new IllegalArgumentException(
                "canonical relocation abort target differs"));
        }
        sources.remove(key, attempt);
        return coordinator.abortPreparedFence(
            new ZLinkAggregateFence(fence.aggregateId(),
                fence.aggregateGeneration()), OPEN);
    }

    public CompletionStage<Void> apply(
        RoutingId transportSource,
        int command,
        byte[] encoded) {
        return apply(transportSource, null, command, encoded)
            .thenApply(ignored -> null);
    }

    @Override
    public CompletionStage<byte[]> apply(
        RoutingId transportSource,
        Long requestSequence,
        int command,
        byte[] encoded) {
        try {
            return switch (command) {
                case ServiceWireConstants.COMMAND_RELOCATION_PREPARE ->
                    onPrepare(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodePrepare(encoded),
                        requestSequence != null);
                case ServiceWireConstants.COMMAND_RELOCATION_READY ->
                    oneWay(requestSequence, onReady(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeReady(encoded)));
                case ServiceWireConstants.COMMAND_RELOCATION_FAILED ->
                    oneWay(requestSequence, onFailed(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeFailed(encoded)));
                case ServiceWireConstants.COMMAND_RELOCATION_DATA ->
                    oneWay(requestSequence, onData(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeData(encoded)));
                case ServiceWireConstants.COMMAND_RELOCATION_CUTOVER ->
                    oneWay(requestSequence, onCutover(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeCutover(encoded)));
                case ServiceWireConstants.COMMAND_RELOCATION_STATE ->
                    oneWay(requestSequence, onState(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeState(encoded)));
                default -> failed(new IllegalArgumentException(
                    "unsupported canonical relocation command"));
            };
        } catch (RuntimeException failure) {
            return failed(failure);
        }
    }

    private static CompletionStage<byte[]> oneWay(
        Long requestSequence,
        CompletionStage<Void> operation) {
        if (requestSequence != null) {
            return failed(new IllegalArgumentException(
                "only canonical relocation prepare is request/reply"));
        }
        return operation.thenApply(ignored -> null);
    }

    private CompletionStage<byte[]> onPrepare(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        boolean request) {
        validateTarget(prepare.target());
        if (!transportSource.equals(prepare.sourceNodeRid())
            || prepare.initiatorRole()
                != ZLinkCanonicalRelocationProtocol.SOURCE) {
            return failed(new IllegalArgumentException(
                "canonical relocation prepare source differs"));
        }
        Fence fence = new Fence(
            prepare.id(), prepare.targetAttemptGeneration());
        TerminalTarget terminal = terminalTargets.get(fence);
        if (terminal != null) {
            if (terminal.source().equals(transportSource)
                && java.util.Arrays.equals(
                    terminal.encodedPrepare(),
                    ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))) {
                LOGGER.warning(
                    "Late or duplicate canonical PREPARE is a no-op: "
                        + fence.id());
                return request
                    ? failed(new IllegalStateException(
                        "terminal canonical relocation prepare cannot reply"))
                    : CompletableFuture.completedFuture(null);
            }
            return failed(new IllegalArgumentException(
                "terminal canonical relocation prepare differs"));
        }
        TargetAttempt created = new TargetAttempt(prepare);
        TargetAttempt current = targets.putIfAbsent(fence, created);
        TargetAttempt attempt = current == null ? created : current;
        if (!java.util.Arrays.equals(
                ZLinkCanonicalRelocationProtocol.encodePrepare(
                    attempt.prepare()),
                ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))) {
            return failed(new IllegalArgumentException(
                "duplicate canonical relocation prepare differs"));
        }
        if (current != null) {
            if (request) {
                return replyReady(fence, attempt);
            }
            publishReady(fence, attempt, transportSource)
                .exceptionally(failure -> {
                    LOGGER.warning("Canonical relocation READY publication "
                        + "failed: " + unwrap(failure));
                    return null;
                });
            return CompletableFuture.completedFuture(null);
        }

        attempt.assembler().assembled()
            .thenCompose(payload -> reconstruct(prepare, payload))
            .thenCompose(restore -> {
                attempt.request().complete(restore.request());
                return target.stage(restore.request())
                    .thenCompose(ignored -> coordinator.prepare(
                        restore.authority(), OPEN))
                    .thenAccept(attempt.prepared()::complete);
            })
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    attempt.ready().complete(null);
                } else {
                    targets.remove(fence, attempt);
                    Throwable cause = unwrap(failure);
                    if (request) {
                        publishFailure(
                            fence, attempt, transportSource, cause, false)
                            .whenComplete((cleanup, cleanupFailure) -> {
                                if (cleanupFailure != null) {
                                    cause.addSuppressed(
                                        unwrap(cleanupFailure));
                                }
                                attempt.ready().completeExceptionally(cause);
                            });
                    } else {
                        attempt.ready().completeExceptionally(cause);
                        publishFailure(
                            fence, attempt, transportSource, cause, true)
                            .exceptionally(publicationFailure -> {
                                LOGGER.warning(
                                    "Canonical relocation failure reply "
                                        + "could not be sent: "
                                        + unwrap(publicationFailure));
                                return null;
                            });
                    }
                }
            });
        // PREPARE is only the ordered registration point.  It must return
        // before relay-ready so command 52 chunks can follow on this same
        // connection; READY or FAILED is published asynchronously after the
        // target finishes assembly and restore.
        if (request) {
            return replyReady(fence, attempt);
        }
        publishReady(fence, attempt, transportSource)
            .exceptionally(failure -> {
                LOGGER.warning("Canonical relocation READY publication failed: "
                    + unwrap(failure));
                return null;
            });
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> publishReady(
        Fence fence,
        TargetAttempt attempt,
        RoutingId source) {
        CompletableFuture<Void> publication;
        synchronized (attempt) {
            if (attempt.readyPublication() != null) {
                return attempt.readyPublication();
            }
            publication = new CompletableFuture<>();
            attempt.readyPublication(publication);
        }
        publication.whenComplete((ignored, failure) -> {
            if (failure != null && !attempt.fallbackArmed()) {
                synchronized (attempt) {
                    //  READY is a one-way submission.  Its transport or
                    //  source-side conflict failure leaves the prepared
                    //  target intact so an exact PREPARE can submit READY
                    //  again with the same fence and RelocationId.
                    if (attempt.readyPublication() == publication) {
                        attempt.readyPublication(null);
                    }
                }
            }
        });
        //  The placeholder is visible before an already-completed ready
        //  stage may run this chain's synchronous prefix and reenter here.
        attempt.ready()
            .thenCompose(ignored -> sendReady(source, attempt.prepare()))
            .thenRun(() -> armCutoverFallback(fence, attempt))
            .exceptionallyCompose(failure ->
                rollbackReadySubmission(fence, attempt, unwrap(failure)))
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    publication.complete(null);
                } else {
                    publication.completeExceptionally(failure);
                }
            });
        return publication;
    }

    private CompletionStage<Void> rollbackReadySubmission(
        Fence fence,
        TargetAttempt attempt,
        Throwable readyFailure) {
        if (attempt.fallbackArmed()) {
            return failed(readyFailure);
        }
        ZLinkAggregateRelocationCoordinator.Prepared prepared =
            completedValue(attempt.prepared());
        if (prepared != null) {
            retentionScheduler.schedule(
                prepared.restoreDeadline(),
                () -> expireReadySubmission(fence, attempt));
        }
        //  Do not abort here.  This is a retryable READY submission failure,
        //  not an explicit pre-relay-ready abort: target.abort removes the
        //  Actor target stage that the exact retry must publish.
        return failed(readyFailure);
    }

    private void expireReadySubmission(Fence fence, TargetAttempt attempt) {
        if (!targets.remove(fence, attempt) || attempt.fallbackArmed()) {
            return;
        }
        ZLinkSpotRetireControl.StageRequest request =
            completedValue(attempt.request());
        if (request != null) {
            target.abort(request).exceptionally(failure -> {
                LOGGER.warning("Canonical relocation READY retry expiry "
                    + "could not discard target stage: " + unwrap(failure));
                return null;
            });
        }
    }

    private static <T> T completedValue(CompletableFuture<T> future) {
        return future.isDone()
                && !future.isCompletedExceptionally()
                && !future.isCancelled()
            ? future.getNow(null)
            : null;
    }

    private static void scheduleAt(Instant deadline, Runnable cleanup) {
        long delay = Math.max(
            1L, Duration.between(Instant.now(), deadline).toMillis());
        CompletableFuture.delayedExecutor(delay, TimeUnit.MILLISECONDS)
            .execute(cleanup);
    }

    private CompletionStage<Void> sendReady(
        RoutingId source,
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        return send(source, ZLinkCanonicalRelocationProtocol.encodeReady(
            new ZLinkCanonicalRelocationProtocol.Ready(
                prepare.id(),
                prepare.targetAttemptGeneration(),
                prepare.coordinator(),
                prepare.target(),
                prepare.object(),
                ZLinkCanonicalRelocationProtocol.TARGET)));
    }

    private CompletionStage<byte[]> replyReady(
        Fence fence,
        TargetAttempt attempt) {
        return attempt.ready().handle((ignored, failure) -> {
            if (failure != null) {
                return ZLinkCanonicalRelocationProtocol.encodeFailed(
                    new ZLinkCanonicalRelocationProtocol.Failed(
                        attempt.prepare().id(),
                        attempt.prepare().targetAttemptGeneration(),
                        attempt.prepare().coordinator(),
                        attempt.prepare().target(),
                        attempt.prepare().object(),
                        ZLinkCanonicalRelocationProtocol.TARGET,
                        wireFailureCode(
                            unwrap(failure),
                            attempt.prepare().object().kind())));
            }
            armCutoverFallback(fence, attempt);
            return ZLinkCanonicalRelocationProtocol.encodeReady(
                new ZLinkCanonicalRelocationProtocol.Ready(
                    attempt.prepare().id(),
                    attempt.prepare().targetAttemptGeneration(),
                    attempt.prepare().coordinator(),
                    attempt.prepare().target(),
                    attempt.prepare().object(),
                    ZLinkCanonicalRelocationProtocol.TARGET));
        });
    }

    private void armCutoverFallback(Fence fence, TargetAttempt attempt) {
        attempt.fallbackArmed(true);
        scheduleCutoverFallback(fence, attempt);
    }

    private CompletionStage<Void> publishFailure(
        Fence fence,
        TargetAttempt attempt,
        RoutingId source,
        Throwable failure,
        boolean sendFailure) {
        ZLinkSpotRetireControl.StageRequest request =
            completedValue(attempt.request());
        CompletionStage<Void> cleanup;
        try {
            cleanup = request == null
                ? CompletableFuture.completedFuture(null)
                : target.abort(request);
        } catch (RuntimeException cleanupFailure) {
            cleanup = CompletableFuture.failedFuture(cleanupFailure);
        }
        long wireFailureCode = wireFailureCode(
            unwrap(failure), attempt.prepare().object().kind());
        return cleanup.handle((ignored, cleanupFailure) -> {
            if (cleanupFailure != null) {
                Throwable cause = unwrap(cleanupFailure);
                failure.addSuppressed(cause);
                LOGGER.warning("Canonical relocation failed-stage cleanup "
                    + "could not complete; sending FAILED reply: " + cause);
            }
            return null;
        }).thenCompose(ignored -> sendFailure
            ? send(source, ZLinkCanonicalRelocationProtocol.encodeFailed(
                new ZLinkCanonicalRelocationProtocol.Failed(
                    attempt.prepare().id(),
                    attempt.prepare().targetAttemptGeneration(),
                    attempt.prepare().coordinator(),
                    attempt.prepare().target(),
                    attempt.prepare().object(),
                    ZLinkCanonicalRelocationProtocol.TARGET,
                    wireFailureCode)))
            : CompletableFuture.completedFuture(null));
    }

    /**
     * Maps a target-side relocation failure's classified
     * {@code ZLinkFrameworkErrorKind} to the closest wire framework-error
     * code the generated schema ({@link ServiceWireConstants}) actually
     * defines. The wire vocabulary predates the framework's typed error
     * kinds and has no one-to-one code for every kind, so several kinds
     * share the nearest fit — documented per case below; unresolvable
     * vocabulary gaps belong at the schema level, not invented here.
     * {@code objectKind} (1 = Actor, else Spot/Instance — spec 28 §4.2's
     * {@code ObjectFence.kind}) picks between an Actor- and Spot-specific
     * code where the schema splits by object kind.
     */
    static long wireFailureCode(Throwable cause, int objectKind) {
        if (!(cause instanceof ZLinkFrameworkException framework)) {
            //  An unclassified throwable carries no evidence of integrity
            //  loss, so it takes the generic opaque request-failure code —
            //  DataLost stays reserved for verified checksum/assembly/digest
            //  failures (spec 15 failure table).
            return ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_FAILED;
        }
        return switch (framework.kind()) {
            case DATA_LOST ->
                ServiceWireConstants.FRAMEWORK_ERROR_RELOCATION_DATA_LOST;
            case REJECTED ->
                ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_REJECTED;
            case PROTOCOL_ERROR ->
                ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_PROTOCOL_ERROR;
            //  No dedicated "capacity exceeded" wire code exists; a full
            //  queue is the closest capacity-shaped signal.
            case CAPACITY_EXCEEDED ->
                ServiceWireConstants.FRAMEWORK_ERROR_WORKER_QUEUE_FULL;
            //  No dedicated "deadline exceeded" wire code exists; a worker
            //  timeout is the closest timeout-shaped signal.
            case DEADLINE_EXCEEDED ->
                ServiceWireConstants.FRAMEWORK_ERROR_WORKER_TIMED_OUT;
            //  A stale generation/fence is the concrete cause of
            //  InvalidOperation along this path (spec 15 failure table);
            //  pick the object-kind-specific stale code.
            case INVALID_OPERATION -> objectKind == 1
                ? ServiceWireConstants.FRAMEWORK_ERROR_ACTOR_LOCATION_STALE
                : ServiceWireConstants.FRAMEWORK_ERROR_SPOT_GENERATION_STALE;
            //  No dedicated generic "unavailable" wire code exists; a
            //  disconnected route is the closest "cannot reach/use the
            //  target" signal.
            case UNAVAILABLE ->
                ServiceWireConstants.FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED;
            case NOT_FOUND ->
                ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_TARGET_NOT_FOUND;
            //  The only "already exists" wire code is Actor-specific; not
            //  expected along this target-failure path, mapped for
            //  completeness.
            case ALREADY_EXISTS ->
                ServiceWireConstants.FRAMEWORK_ERROR_ACTOR_ALREADY_EXISTS;
            case TYPE_MISMATCH -> objectKind == 1
                ? ServiceWireConstants.FRAMEWORK_ERROR_ACTOR_TYPE_MISMATCH
                : ServiceWireConstants.FRAMEWORK_ERROR_SPOT_TYPE_MISMATCH;
            //  No dedicated "not configured" wire code exists; a missing
            //  configured handler is the closest analog.
            case NOT_CONFIGURED ->
                ServiceWireConstants.FRAMEWORK_ERROR_HANDLER_NOT_FOUND;
            //  No dedicated generic "internal failure" or "shutting down"
            //  wire code exists; the generic opaque request-failure code is
            //  the closest fit for both.
            case INTERNAL_FAILURE, SHUTTING_DOWN ->
                ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_FAILED;
        };
    }

    /**
     * Inverse of {@link #wireFailureCode(Throwable, int)}: maps a received
     * {@code relocationFailed(53)} wire failure code back to the framework
     * error kind the emitting target classified, so a source-side rejection
     * carries the same typed classification in every language (node and cpp
     * decode identically). Where the emit table collapses several kinds into
     * one code the decode picks the kind the emit table documents as the
     * code's primary meaning; both object-kind variants of a split code
     * (Actor/Spot stale and type-mismatch) decode to the same kind. Any
     * unknown or unmapped code falls back to {@code INTERNAL_FAILURE} —
     * the same fail-safe generic classification as before (spec 15
     * §"Failed.Kind").
     */
    static ZLinkFrameworkErrorKind wireFailureKind(long failureCode) {
        return switch ((int) failureCode) {
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_RELOCATION_DATA_LOST ->
                ZLinkFrameworkErrorKind.DATA_LOST;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_REJECTED ->
                ZLinkFrameworkErrorKind.REJECTED;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_PROTOCOL_ERROR ->
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_WORKER_QUEUE_FULL ->
                ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_WORKER_TIMED_OUT ->
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_ACTOR_LOCATION_STALE,
                 (int) ServiceWireConstants.FRAMEWORK_ERROR_SPOT_GENERATION_STALE ->
                ZLinkFrameworkErrorKind.INVALID_OPERATION;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED ->
                ZLinkFrameworkErrorKind.UNAVAILABLE;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_REQUEST_TARGET_NOT_FOUND ->
                ZLinkFrameworkErrorKind.NOT_FOUND;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_ACTOR_ALREADY_EXISTS ->
                ZLinkFrameworkErrorKind.ALREADY_EXISTS;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_ACTOR_TYPE_MISMATCH,
                 (int) ServiceWireConstants.FRAMEWORK_ERROR_SPOT_TYPE_MISMATCH ->
                ZLinkFrameworkErrorKind.TYPE_MISMATCH;
            case (int) ServiceWireConstants.FRAMEWORK_ERROR_HANDLER_NOT_FOUND ->
                ZLinkFrameworkErrorKind.NOT_CONFIGURED;
            default ->
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
        };
    }

    private CompletionStage<Void> onReady(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Ready ready) {
        Fence fence = new Fence(
            ready.id(), ready.targetAttemptGeneration());
        SourceAttempt attempt = sources.get(fence);
        if (attempt == null) {
            return CompletableFuture.completedFuture(null);
        }
        var prepare = attempt.prepare();
        if (!transportSource.equals(prepare.target().nodeRid())
            || !ready.coordinator().equals(prepare.coordinator())
            || !ready.target().equals(prepare.target())
            || !ready.object().equals(prepare.object())
            || ready.senderRole() != ZLinkCanonicalRelocationProtocol.TARGET) {
            return failed(new IllegalArgumentException(
                "canonical relocation ready fence differs"));
        }
        attempt.ready().complete(null);
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> onFailed(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Failed failure) {
        Fence fence = new Fence(
            failure.id(), failure.targetAttemptGeneration());
        SourceAttempt attempt = sources.get(fence);
        if (attempt == null) {
            return CompletableFuture.completedFuture(null);
        }
        var prepare = attempt.prepare();
        if (!transportSource.equals(prepare.target().nodeRid())
            || !failure.coordinator().equals(prepare.coordinator())
            || !failure.target().equals(prepare.target())
            || !failure.object().equals(prepare.object())
            || failure.senderRole() != ZLinkCanonicalRelocationProtocol.TARGET) {
            return failed(new IllegalArgumentException(
                "canonical relocation failure fence differs"));
        }
        //  Decode the wire failure code back to the typed framework error
        //  kind the target classified, mirroring node/cpp, so the caller
        //  sees the same public classification in every language.
        attempt.ready().completeExceptionally(new ZLinkFrameworkException(
            wireFailureKind(failure.failureCode()),
            "target rejected canonical relocation: " + failure.failureCode()));
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> onState(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.State state) {
        Fence fence = new Fence(state.id(), state.targetAttemptGeneration());
        TargetAttempt attempt = targets.get(fence);
        if (attempt == null
            || !attempt.prepare().sourceNodeRid().equals(transportSource)
            || !state.coordinator().equals(attempt.prepare().coordinator())
            || !state.object().equals(attempt.prepare().object())) {
            //  A chunk with a different exact identity is discarded and never
            //  connected to a running assembly (spec 28 §4.3).
            return CompletableFuture.completedFuture(null);
        }
        //  The assembler copies the chunk before this turn returns, so the
        //  transport buffer never outlives the call (spec 28 §4.3).
        attempt.assembler().accept(state.chunkOrdinal(), state.chunkData());
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> onData(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Data data) {
        TargetAttempt attempt = requireTarget(
            new Fence(data.id(), data.targetAttemptGeneration()),
            transportSource);
        if (!data.coordinator().equals(attempt.prepare().coordinator())
            || !data.object().equals(attempt.prepare().object())) {
            return failed(new IllegalArgumentException(
                "canonical relocation data fence differs"));
        }
        attempt.boundary().append(data.frozenRecord());
        return attempt.ready().thenCompose(ignored -> attempt.request()
            .thenCompose(request -> target.stageRelayedRecord(
                request, data.frozenRecord())));
    }

    private CompletionStage<Void> onCutover(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Cutover cutover) {
        Fence fence = new Fence(
            cutover.id(), cutover.targetAttemptGeneration());
        TargetAttempt attempt = targets.get(fence);
        if (attempt == null) {
            TerminalTarget terminal = terminalTargets.get(fence);
            if (terminal != null && terminal.source().equals(transportSource)
                && (terminal.encodedCutover() == null
                    || java.util.Arrays.equals(
                        terminal.encodedCutover(),
                        ZLinkCanonicalRelocationProtocol.encodeCutover(
                            cutover)))) {
                LOGGER.warning(
                    "Late or duplicate canonical CUTOVER is a no-op: "
                        + fence.id());
                return CompletableFuture.completedFuture(null);
            }
            return failed(new IllegalStateException(
                "canonical target relocation is unavailable"));
        }
        if (!attempt.prepare().sourceNodeRid().equals(transportSource)) {
            return failed(new IllegalArgumentException(
                "canonical relocation cutover source differs"));
        }
        if (!cutover.coordinator().equals(attempt.prepare().coordinator())
            || !cutover.object().equals(attempt.prepare().object())
            || cutover.senderRole()
                != ZLinkCanonicalRelocationProtocol.SOURCE) {
            return failed(new IllegalArgumentException(
                "canonical relocation cutover fence differs"));
        }
        RelayBoundary boundary = attempt.boundary();
        if (boundary.recordCount() != cutover.boundaryRecordCount()
            || boundary.checksumCrc32c()
                != cutover.boundaryChecksumCrc32c()) {
            //  Replacement by a retransmitted batch: discard the partially
            //  received pre-boundary section and wait for the whole batch —
            //  on an ordered connection a mismatch at first receipt is a
            //  defect signal (spec 28 §4.4).
            LOGGER.severe(
                "Canonical CUTOVER boundary record count or checksum differs"
                    + " from the received relay section: " + fence.id());
            return failed(new IllegalStateException(
                "canonical relocation cutover boundary differs"));
        }
        attempt.receivedCutover(
            ZLinkCanonicalRelocationProtocol.encodeCutover(cutover));
        return publishTarget(fence, attempt, false);
    }

    private void scheduleCutoverFallback(
        Fence fence, TargetAttempt attempt) {
        CompletableFuture.delayedExecutor(
                transferOptions.cutoverWaitTimeout().toMillis(),
                TimeUnit.MILLISECONDS)
            .execute(() -> publishTarget(fence, attempt, true)
                .exceptionally(ignored -> null));
    }

    private CompletionStage<Void> publishTarget(
        Fence fence,
        TargetAttempt attempt,
        boolean fallback) {
        CompletableFuture<Void> publication;
        synchronized (attempt) {
            if (attempt.publication() != null) {
                return attempt.publication();
            }
            if (fallback) {
                //  The cutover wait elapsed without a cutover or retransmit.
                LOGGER.warning(
                    "cutover_timeout: relay-ready wait elapsed without a"
                        + " CUTOVER; continuing with target-only CAS: "
                        + fence.id());
                ZLinkRuntimeMetrics.increment(
                    "zlink.relocation.cutover_timeout", Map.of());
            }
            publication = new CompletableFuture<>();
            attempt.publication(publication);
        }
        publication.whenComplete((ignored, failure) -> {
            if (failure != null && !attempt.committed()) {
                attempt.request().thenCompose(target::abort)
                    .whenComplete((discarded, discardFailure) ->
                        targets.remove(fence, attempt));
            } else {
                retainTerminalTarget(fence, attempt);
            }
        });
        //  Publish the claim before completed prepare/commit stages can run
        //  inline and make the target attempt observable again.
        attempt.prepared()
            .thenCompose(this::commitUntilRestoreExpiry)
            .thenApply(published -> {
                //  S2 — target owner CAS confirmed.
                attempt.committed(true);
                attempt.committedNanos(System.nanoTime());
                return published;
            })
            .thenCompose(ignored -> attempt.request())
            .thenCompose(target::publish)
            .thenApply(ignored -> {
                //  S3 — application dispatch opened on the target.
                long committedNanos = attempt.committedNanos();
                if (committedNanos != 0) {
                    ZLinkRuntimeMetrics.record(
                        "zlink.relocation.target_resume",
                        Duration.ofNanos(
                            System.nanoTime() - committedNanos),
                        Map.of());
                }
                return ignored;
            })
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    publication.complete(null);
                } else {
                    publication.completeExceptionally(failure);
                }
            });
        return publication;
    }

    private void retainTerminalTarget(Fence fence, TargetAttempt attempt) {
        ZLinkAggregateRelocationCoordinator.Prepared prepared =
            attempt.prepared().join();
        var prepare = attempt.prepare();
        TerminalTarget terminal = new TerminalTarget(
            prepare.sourceNodeRid(),
            ZLinkCanonicalRelocationProtocol.encodePrepare(prepare),
            attempt.receivedCutover());
        TerminalTarget current = terminalTargets.putIfAbsent(fence, terminal);
        if (current == null) {
            retentionScheduler.schedule(
                prepared.restoreDeadline(),
                () -> terminalTargets.remove(fence, terminal));
        }
        targets.remove(fence, attempt);
    }

    private CompletionStage<ZLinkAggregateRelocationCoordinator.Published>
        commitUntilRestoreExpiry(
            ZLinkAggregateRelocationCoordinator.Prepared prepared) {
        CompletableFuture<ZLinkAggregateRelocationCoordinator.Published>
            result = new CompletableFuture<>();
        commitUntilRestoreExpiry(prepared, result);
        return result;
    }

    private void commitUntilRestoreExpiry(
        ZLinkAggregateRelocationCoordinator.Prepared prepared,
        CompletableFuture<ZLinkAggregateRelocationCoordinator.Published>
            result) {
        if (!Instant.now().isBefore(prepared.restoreDeadline())) {
            result.completeExceptionally(new TimeoutException(
                "relocation Restore validity expired before target owner CAS"));
            return;
        }
        coordinator.commit(prepared, OPEN).whenComplete((published, failure) -> {
            if (failure == null) {
                result.complete(published);
                return;
            }
            Throwable cause = unwrap(failure);
            if (cause instanceof ZLinkAggregateRelocationCoordinator
                    .AuthorityConflictException
                || cause instanceof ZLinkAggregateRelocationCoordinator
                    .RelocationDataLostException
                || !Instant.now().isBefore(prepared.restoreDeadline())) {
                result.completeExceptionally(cause);
                return;
            }
            long remainingMillis = Math.max(1L, Duration.between(
                Instant.now(), prepared.restoreDeadline()).toMillis());
            CompletableFuture.delayedExecutor(
                    Math.min(STORE_RETRY_DELAY.toMillis(), remainingMillis),
                    TimeUnit.MILLISECONDS)
                .execute(() -> commitUntilRestoreExpiry(prepared, result));
        });
    }

    private CompletionStage<ZLinkCanonicalRelocationProtocol.Prepare>
        sourcePrepare(ZLinkSpotRetireControl.StageRequest request) {
        ZLinkSpotRetireControl.ParticipantFence primary =
            primary(request.participants());
        CompletionStage<ZLinkAuthorityReadResult> authority =
            locations.read(primary.authorityKey(), OPEN);
        return authority.thenApply(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || snapshot.objectGeneration() != primary.objectGeneration()
                || snapshot.authorityOwnerGeneration()
                    != primary.sourceAuthorityOwnerGeneration()
                || !snapshot.ownerId().equals(request.sourceOwnerId())
                || snapshot.ownerLeaseGeneration()
                    != request.sourceOwnerLeaseGeneration()) {
                throw new IllegalStateException(
                    "source relocation authority fence is stale");
            }
            byte[] payload = request.relocationPayload();
            var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(
                payload);
            return new ZLinkCanonicalRelocationProtocol.Prepare(
                request.fence().aggregateId(),
                request.fence().aggregateGeneration(),
                new ZLinkCanonicalRelocationProtocol.Coordinator(
                    request.sourceOwnerId(),
                    request.sourceOwnerLeaseGeneration(),
                    request.sourceNodeRid(),
                    request.sourceNodeGeneration(),
                    snapshot.storeVersion()),
                new ZLinkCanonicalRelocationProtocol.Target(
                    request.targetNodeRid(),
                    request.targetNodeGeneration(),
                    request.targetOwnerId(),
                    request.targetOwnerLeaseGeneration()),
                ZLinkCanonicalRelocationProtocol.SOURCE,
                new ZLinkCanonicalRelocationProtocol.ObjectFence(
                    primary.objectKind(),
                    primary.objectId(),
                    "",
                    primary.objectGeneration(),
                    primary.sourceAuthorityOwnerGeneration()),
                request.sourceNodeRid(),
                request.sourceNodeGeneration(),
                ZLinkRelocationPayloadTransfer.manifest(
                    payload,
                    budget.effectiveChunkBytes(
                        request.advertisedReceiveChunkLimitBytes())),
                envelope.applicationVersion());
        });
    }

    private CompletionStage<TargetRestore> reconstruct(
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        byte[] payload) {
        Objects.requireNonNull(payload, "payload");
        CompletionStage<List<ZLinkAuthorityEntry>> inventory =
            prepare.object().kind() == 1
                ? readStandaloneActor(prepare)
                : readUserSpotInventory(prepare);
        return inventory.thenApply(entries -> {
            ZLinkSpotRetireControl.TargetProfile profile =
                Objects.requireNonNull(
                    target.applyTargetProfile(stageRequest(
                        prepare, payload, entries),
                        localNodeGeneration),
                    "canonical target profile returned null");
            ZLinkSpotRetireControl.StageRequest request = profile.request();
            return new TargetRestore(
                request,
                authorityRequest(
                    prepare, payload, entries, profile));
        });
    }

    private ZLinkAggregateRelocationCoordinator.Request authorityRequest(
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        byte[] root,
        List<ZLinkAuthorityEntry> entries,
        ZLinkSpotRetireControl.TargetProfile profile) {
        ZLinkSpotRetireControl.StageRequest request = profile.request();
        boolean standalone = prepare.object().kind()
            == ZLinkPlacementObjectKind.ACTOR.value();
        List<ZLinkAggregateRelocationCoordinator.Participant> participants =
            new ArrayList<>(entries.size());
        var actorCodec = new ZLinkActorAuthorityPayloadCodec();
        for (ZLinkAuthorityEntry entry : entries) {
            ZLinkAuthoritySnapshot snapshot = entry.snapshot();
            byte[] authorityPayload = snapshot.payload();
            byte[] membershipMutation = new byte[0];
            if (standalone) {
                String actorId = ZLinkAuthorityKeyCodec.decode(entry.key()).id();
                authorityPayload = actorCodec.encode(
                    ZLinkActorAuthorityPayloadCodec.State.READY,
                    snapshot.allocation().stableType(),
                    actorId,
                    request.spotId(),
                    profile.actorSpotGeneration(),
                    profile.actorSpotKind(),
                    request.targetOwnerId(),
                    request.targetOwnerLeaseGeneration(),
                    meshName,
                    localNodeRid,
                    localNodeGeneration);
            }
            participants.add(new ZLinkAggregateRelocationCoordinator
                .Participant(
                    entry.key(),
                    snapshot.allocation().objectKind(),
                    snapshot.objectGeneration(),
                    snapshot.authorityOwnerGeneration(),
                    snapshot.storeVersion(),
                    ZLinkAuthorityGenerationTransition.NEW_OWNER,
                    authorityPayload,
                    membershipMutation));
        }
        long actorCount = participants.stream()
            .filter(value -> value.objectKind()
                == ZLinkPlacementObjectKind.ACTOR)
            .count();
        ZLinkPlacementCapacityBundle capacity = standalone
            ? ZLinkPlacementCapacityBundle.actor(1)
            : new ZLinkPlacementCapacityBundle(
                Math.toIntExact(actorCount),
                1,
                Optional.of(new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    request.stableType(),
                    1)));
        return new ZLinkAggregateRelocationCoordinator.Request(
            prepare.id(),
            request.fence().aggregateGeneration(),
            prepare.targetAttemptGeneration(),
            participants,
            root,
            new ZLinkMeshNodeDescriptorKey(meshName, localNodeRid),
            localNodeGeneration,
            capacity,
            new ZLinkLocationOwnerToken(
                request.targetOwnerId(),
                request.targetOwnerLeaseGeneration()),
            prepare.coordinator().expectedAuthorityStoreVersion());
    }

    private CompletionStage<List<ZLinkAuthorityEntry>> readStandaloneActor(
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        String key = ZLinkAuthorityKeyCodec.actor(prepare.object().objectId());
        return locations.read(key, OPEN).thenApply(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                throw new IllegalStateException(
                    "standalone Actor authority is missing");
            }
            return List.of(new ZLinkAuthorityEntry(key, snapshot));
        });
    }

    private CompletionStage<List<ZLinkAuthorityEntry>> readUserSpotInventory(
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        String key = ZLinkAuthorityKeyCodec.spot(prepare.object().objectId());
        return locations.read(key, OPEN).thenCompose(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                return failed(new IllegalStateException(
                    "User Spot authority is missing"));
            }
            return scanActors(Optional.empty(), new ArrayList<>())
                .thenApply(actors -> {
                    List<ZLinkAuthorityEntry> values = new ArrayList<>();
                    values.add(new ZLinkAuthorityEntry(key, snapshot));
                    var actorCodec = new ZLinkActorAuthorityPayloadCodec();
                    for (ZLinkAuthorityEntry actor : actors) {
                        var authority = actorCodec.decode(
                            actor.snapshot().payload());
                        if (authority.isPresent()
                            && authority.get().currentSpotId().equals(
                                prepare.object().objectId())) {
                            values.add(actor);
                        }
                    }
                    values.sort(Comparator.comparing(
                        ZLinkAuthorityEntry::key,
                        ZLinkCanonicalRelocationStateMachine::compareUtf8));
                    return List.copyOf(values);
                });
        });
    }

    private CompletionStage<List<ZLinkAuthorityEntry>> scanActors(
        Optional<ZLinkAuthorityScanCursor> cursor,
        List<ZLinkAuthorityEntry> collected) {
        return locations.list(
                ACTOR_AUTHORITY_PREFIX, cursor, SCAN_PAGE_SIZE, OPEN)
            .thenCompose(result -> {
                if (result instanceof ZLinkAuthorityScanExpired) {
                    return failed(new IllegalStateException(
                        "Actor authority scan expired during relocation"));
                }
                ZLinkAuthorityPage page = (ZLinkAuthorityPage) result;
                collected.addAll(page.items());
                return page.nextCursor().isEmpty()
                    ? CompletableFuture.completedFuture(List.copyOf(collected))
                    : scanActors(page.nextCursor(), collected);
            });
    }

    private ZLinkSpotRetireControl.StageRequest stageRequest(
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        byte[] rootBytes,
        List<ZLinkAuthorityEntry> entries) {
        var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(rootBytes);
        if (envelope.relocationHigh()
                != prepare.id().getMostSignificantBits()
            || envelope.relocationLow()
                != prepare.id().getLeastSignificantBits()
            || envelope.object().kind() != prepare.object().kind()
            || !envelope.object().objectId().equals(
                prepare.object().objectId())
            || envelope.object().objectGeneration()
                != prepare.object().objectGeneration()
            || envelope.object().expectedAuthorityOwnerGeneration()
                != prepare.object().expectedAuthorityOwnerGeneration()
            || envelope.applicationVersion() != prepare.applicationVersion()
            || entries.size() != envelope.applicationStates().size()) {
            throw new IllegalStateException(
                "target relocation inventory differs from root"
                    + relocationInventoryDiagnostic(prepare, envelope, entries));
        }
        Map<Long, Boolean> restore = new HashMap<>();
        for (var state : envelope.applicationStates()) {
            restore.put(state.participantId(), state.hasState());
        }
        List<ZLinkSpotRetireControl.ParticipantFence> participants =
            new ArrayList<>(entries.size());
        var spotCodec = new ZLinkServiceAuthorityPayloadCodec();
        String stableType = null;
        String targetSpotId = entrySpotId;
        boolean restorePrimary = false;
        ZLinkAuthoritySnapshot primarySnapshot = null;
        for (int index = 0; index < entries.size(); index++) {
            ZLinkAuthorityEntry entry = entries.get(index);
            ZLinkAuthoritySnapshot snapshot = entry.snapshot();
            boolean hasState = Boolean.TRUE.equals(restore.get(index + 1L));
            if (!snapshot.ownerId().equals(prepare.coordinator().ownerId())
                || snapshot.ownerLeaseGeneration()
                    != prepare.coordinator().ownerLeaseGeneration()
                || !snapshot.allocation().descriptor().meshName().equals(
                    meshName)
                || !snapshot.allocation().descriptor().rid().equals(
                    prepare.sourceNodeRid())
                || snapshot.allocation().descriptorLifecycleGeneration()
                    != prepare.sourceNodeGeneration()) {
                throw new IllegalStateException(
                    "target relocation source authority fence differs");
            }
            int kind = snapshot.allocation().objectKind().value();
            String objectId;
            String objectType;
            if (kind == 2) {
                var decoded = spotCodec.decode(snapshot.payload())
                    .orElseThrow(() -> new IllegalStateException(
                        "User Spot authority payload is invalid"));
                objectId = decoded.spotId();
                objectType = decoded.stableType();
                targetSpotId = objectId;
            } else {
                objectId = ZLinkAuthorityKeyCodec.decode(entry.key()).id();
                objectType = snapshot.allocation().stableType();
            }
            participants.add(new ZLinkSpotRetireControl.ParticipantFence(
                entry.key(),
                kind,
                objectId,
                objectType,
                hasState,
                snapshot.objectGeneration(),
                snapshot.authorityOwnerGeneration()));
            if (kind == prepare.object().kind()
                && objectId.equals(prepare.object().objectId())) {
                stableType = objectType;
                restorePrimary = hasState;
                primarySnapshot = snapshot;
            }
        }
        validateResolvedPrimary(prepare, participants);
        if (primarySnapshot == null) {
            throw new IllegalStateException(
                "relocation coordinator authority version differs");
        }
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(
                prepare.id(), prepare.targetAttemptGeneration()),
            prepare.sourceNodeRid(),
            prepare.sourceNodeGeneration(),
            prepare.coordinator().ownerId(),
            prepare.coordinator().ownerLeaseGeneration(),
            prepare.target().nodeRid(),
            prepare.target().nodeGeneration(),
            prepare.target().ownerId(),
            prepare.target().ownerLeaseGeneration(),
            meshName,
            targetSpotId,
            stableType,
            false,
            restorePrimary,
            rootBytes,
            participants,
            List.of());
    }

    private static String relocationInventoryDiagnostic(
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        ZLinkServiceRelocationEnvelopeCodec.Envelope envelope,
        List<ZLinkAuthorityEntry> entries) {
        StringBuilder value = new StringBuilder(" [root={id=")
            .append(envelope.relocationHigh()).append(':')
            .append(envelope.relocationLow()).append(",kind=")
            .append(envelope.object().kind()).append(",objectId=")
            .append(envelope.object().objectId()).append(",objectGeneration=")
            .append(envelope.object().objectGeneration())
            .append(",authorityOwnerGeneration=")
            .append(envelope.object().expectedAuthorityOwnerGeneration())
            .append(",applicationVersion=")
            .append(envelope.applicationVersion()).append(",states=")
            .append(envelope.applicationStates().size()).append("},prepare={id=")
            .append(prepare.id().getMostSignificantBits()).append(':')
            .append(prepare.id().getLeastSignificantBits()).append(",kind=")
            .append(prepare.object().kind()).append(",objectId=")
            .append(prepare.object().objectId()).append(",stableType=")
            .append(prepare.object().stableType()).append(",objectGeneration=")
            .append(prepare.object().objectGeneration())
            .append(",authorityOwnerGeneration=")
            .append(prepare.object().expectedAuthorityOwnerGeneration())
            .append(",applicationVersion=")
            .append(prepare.applicationVersion()).append("},authorities=[");
        var spotCodec = new ZLinkServiceAuthorityPayloadCodec();
        for (int index = 0; index < entries.size(); index++) {
            if (index != 0) {
                value.append(';');
            }
            var entry = entries.get(index);
            var snapshot = entry.snapshot();
            int kind = snapshot.allocation().objectKind().value();
            String objectId = "<invalid>";
            String stableType = "<invalid>";
            if (kind == 2) {
                var decoded = spotCodec.decode(snapshot.payload());
                if (decoded.isPresent()) {
                    objectId = decoded.get().spotId();
                    stableType = decoded.get().stableType();
                }
            } else if (kind == 1) {
                objectId = ZLinkAuthorityKeyCodec.decode(entry.key()).id();
                stableType = snapshot.allocation().stableType();
            }
            value.append("{index=").append(index + 1)
                .append(",keyUtf8Hex=").append(java.util.HexFormat.of()
                    .formatHex(entry.key().getBytes(StandardCharsets.UTF_8)))
                .append(",key=").append(entry.key()).append(",kind=")
                .append(kind).append(",objectId=").append(objectId)
                .append(",stableType=").append(stableType)
                .append(",objectGeneration=")
                .append(snapshot.objectGeneration())
                .append(",authorityOwnerGeneration=")
                .append(snapshot.authorityOwnerGeneration())
                .append(",storeVersion=").append(snapshot.storeVersion())
                .append('}');
        }
        return value.append("]]" ).toString();
    }

    private static void validateResolvedPrimary(
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        List<ZLinkSpotRetireControl.ParticipantFence> participants) {
        ZLinkSpotRetireControl.ParticipantFence primary = participants.stream()
            .filter(value -> value.objectKind() == prepare.object().kind()
                && value.objectId().equals(prepare.object().objectId()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException(
                "relocation primary authority is missing"));
        if (primary.objectGeneration()
                != prepare.object().objectGeneration()
            || primary.sourceAuthorityOwnerGeneration()
                != prepare.object().expectedAuthorityOwnerGeneration()) {
            throw new IllegalStateException(
                "relocation primary authority generation differs");
        }
    }

    private void validateTarget(
        ZLinkCanonicalRelocationProtocol.Target targetFence) {
        if (!targetFence.nodeRid().equals(localNodeRid)
            || targetFence.nodeGeneration() != localNodeGeneration) {
            throw new IllegalArgumentException(
                "canonical relocation target node fence is stale");
        }
    }

    private SourceAttempt requireSource(Fence fence, RoutingId targetRid) {
        SourceAttempt attempt = sources.get(fence);
        if (attempt == null
            || !attempt.request().targetNodeRid().equals(targetRid)) {
            throw new IllegalStateException(
                "canonical source relocation is unavailable");
        }
        return attempt;
    }

    private TargetAttempt requireTarget(Fence fence, RoutingId sourceRid) {
        TargetAttempt attempt = targets.get(fence);
        if (attempt == null
            || !attempt.prepare().sourceNodeRid().equals(sourceRid)) {
            throw new IllegalStateException(
                "canonical target relocation is unavailable");
        }
        return attempt;
    }

    private CompletionStage<Void> send(
        RoutingId targetRid, byte[] command) {
        return node.sendCanonicalRelocationControl(targetRid, command);
    }

    private static ZLinkSpotRetireControl.ParticipantFence primary(
        List<ZLinkSpotRetireControl.ParticipantFence> participants) {
        return participants.stream()
            .filter(value -> value.objectKind() == 2)
            .findFirst()
            .orElse(participants.getFirst());
    }

    private static int compareUtf8(String left, String right) {
        return java.util.Arrays.compareUnsigned(
            left.getBytes(StandardCharsets.UTF_8),
            right.getBytes(StandardCharsets.UTF_8));
    }

    private static String requireText(String value, String field) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(field + " must not be blank");
        }
        return value;
    }

    private static void requireTimeout(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isZero() || timeout.isNegative()) {
            throw new IllegalArgumentException(
                "relocation timeout must be positive");
        }
    }

    /**
     * Spec 15 §4.3 — the source's Actor Restore request is retried within
     * the Join deadline while the same target process is running, and a
     * duplicate PREPARE is idempotent on the target. The request reply can be
     * lost while symmetric manual duplicate connections converge (spec 07
     * §518), so waiting the whole deadline on one submission turns a lost
     * reply into a silent Join stall. Resend the idempotent PREPARE each
     * bounded slice until relay readiness or the remaining deadline is spent.
     */
    private CompletionStage<Void> awaitReadyWithPrepareResend(
        RoutingId targetNodeRid,
        SourceAttempt attempt,
        long deadlineNanos,
        boolean submitFirst) {
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0) {
            return failed(new TimeoutException(
                "relocation relay readiness timed out"));
        }
        Duration slice = Duration.ofNanos(
            Math.min(remainingNanos, TimeUnit.SECONDS.toNanos(1)));
        CompletionStage<Void> submitted = CompletableFuture.completedFuture(null);
        if (submitFirst && !attempt.ready().isDone()) {
            if (node instanceof ZLinkInternalMeshNode
                    .CanonicalRelocationPrepareRequestReplySupport) {
                CompletionStage<byte[]> requestReply =
                    node.requestCanonicalRelocationPrepare(
                    targetNodeRid,
                    ZLinkCanonicalRelocationProtocol.encodePrepare(
                        attempt.prepare()),
                    slice);
                CompletionStage<Void> reply = requestReply
                    .thenCompose(encoded -> apply(
                        targetNodeRid,
                        Byte.toUnsignedInt(encoded[3]),
                        encoded));
                reply.whenComplete((ignored, failure) -> {
                    if (failure != null
                        && !(unwrap(failure) instanceof TimeoutException)) {
                        attempt.ready().completeExceptionally(unwrap(failure));
                    }
                });
            } else {
                submitted = send(targetNodeRid,
                    ZLinkCanonicalRelocationProtocol.encodePrepare(
                        attempt.prepare()));
            }
            // The request is now in flight; command 52 may follow before the
            // target completes the reply leg.
            submitted = submitted.thenCompose(ignored ->
                sendStateChunks(targetNodeRid, attempt));
        }
        return submitted
            .exceptionallyCompose(failure -> failed(unwrap(failure)))
            .thenCompose(ignored -> timed(
                    attempt.ready(), slice, "relocation relay readiness")
                .handle((ready, failure) -> {
                    if (failure == null) {
                        return CompletableFuture.<Void>completedFuture(null);
                    }
                    if (unwrap(failure) instanceof TimeoutException
                        && deadlineNanos - System.nanoTime() > 0) {
                        return awaitReadyWithPrepareResend(
                            targetNodeRid, attempt, deadlineNanos, true)
                            .toCompletableFuture();
                    }
                    return CompletableFuture.<Void>failedFuture(unwrap(failure));
                })
                .thenCompose(stage -> stage));
    }

    /**
     * Streams the captured payload as command 52 chunks on the same ordered
     * connection, one submission at a time, each charged against the
     * in-flight payload budget until its transport terminal (spec 28 §4.2,
     * §5.3). A resent PREPARE resends the chunks; identical chunks are
     * idempotent on the target.
     */
    private CompletionStage<Void> sendStateChunks(
        RoutingId targetNodeRid,
        SourceAttempt attempt) {
        if (attempt.ready().isDone()) {
            return CompletableFuture.completedFuture(null);
        }
        var prepare = attempt.prepare();
        List<byte[]> chunks = ZLinkRelocationPayloadTransfer.chunks(
            attempt.request().relocationPayload(),
            budget.effectiveChunkBytes(
                attempt.request().advertisedReceiveChunkLimitBytes()));
        CompletionStage<Void> chain =
            CompletableFuture.completedFuture(null);
        for (int index = 0; index < chunks.size(); index++) {
            byte[] chunk = chunks.get(index);
            long ordinal = index;
            chain = chain.thenCompose(ignored ->
                budget.acquire(targetNodeRid, chunk.length)
                    .thenCompose(admitted -> send(
                            targetNodeRid,
                            ZLinkCanonicalRelocationProtocol.encodeState(
                                new ZLinkCanonicalRelocationProtocol.State(
                                    prepare.id(),
                                    prepare.targetAttemptGeneration(),
                                    prepare.coordinator(),
                                    ZLinkCanonicalRelocationProtocol.SOURCE,
                                    prepare.object(),
                                    ordinal,
                                    chunk)))
                        .whenComplete((result, failure) ->
                            budget.release(targetNodeRid, chunk.length))));
        }
        return chain;
    }

    private static CompletionStage<Void> timed(
        CompletionStage<Void> stage,
        Duration timeout,
        String operation) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        stage.whenComplete((ignored, failure) -> {
            if (failure == null) {
                result.complete(null);
            } else {
                result.completeExceptionally(unwrap(failure));
            }
        });
        CompletableFuture.delayedExecutor(
                timeout.toMillis(), TimeUnit.MILLISECONDS)
            .execute(() -> result.completeExceptionally(
                new TimeoutException(operation + " timed out")));
        return result;
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    private record Fence(UUID id, long attempt) {
        static Fence from(ZLinkSpotRetireControl.Fence fence) {
            return new Fence(fence.aggregateId(), fence.aggregateGeneration());
        }
    }

    private record SourceAttempt(
        ZLinkSpotRetireControl.StageRequest request,
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        CompletableFuture<Void> ready,
        AtomicInteger activeWaiters,
        RelayBatch batch) {
        SourceAttempt(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkCanonicalRelocationProtocol.Prepare prepare) {
            this(
                request,
                prepare,
                new CompletableFuture<>(),
                new AtomicInteger(),
                new RelayBatch());
        }
    }

    /**
     * Source-side pre-boundary relay batch: the retransmission copy plus the
     * running record count and CRC-32C the cutover carries (spec 28 §4.4).
     */
    private static final class RelayBatch {
        private final List<byte[]> encodedFrames = new ArrayList<>();
        private final java.util.zip.CRC32C checksum =
            new java.util.zip.CRC32C();
        private long recordCount;

        synchronized void append(byte[] frozenRecord, byte[] encodedFrame) {
            checksum.update(frozenRecord, 0, frozenRecord.length);
            recordCount++;
            encodedFrames.add(encodedFrame.clone());
        }

        synchronized long recordCount() {
            return recordCount;
        }

        synchronized long checksumCrc32c() {
            return checksum.getValue();
        }

        synchronized List<byte[]> encodedFrames() {
            return List.copyOf(encodedFrames);
        }
    }

    /**
     * Target-side pre-boundary relay accounting the cutover is checked
     * against (spec 28 §4.4).
     */
    private static final class RelayBoundary {
        private final java.util.zip.CRC32C checksum =
            new java.util.zip.CRC32C();
        private long recordCount;

        synchronized void append(byte[] frozenRecord) {
            checksum.update(frozenRecord, 0, frozenRecord.length);
            recordCount++;
        }

        synchronized long recordCount() {
            return recordCount;
        }

        synchronized long checksumCrc32c() {
            return checksum.getValue();
        }
    }

    /** Retained copies for the cutover retransmission window (spec 28 §4.4). */
    private record RetainedSource(
        RoutingId targetNodeRid,
        List<byte[]> dataFrames,
        byte[] encodedCutover) {
        private RetainedSource {
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            dataFrames = List.copyOf(dataFrames);
            encodedCutover = encodedCutover.clone();
        }

        @Override public byte[] encodedCutover() {
            return encodedCutover.clone();
        }
    }

    private record TargetRestore(
        ZLinkSpotRetireControl.StageRequest request,
        ZLinkAggregateRelocationCoordinator.Request authority) {
    }

    private record TerminalTarget(
        RoutingId source,
        byte[] encodedPrepare,
        byte[] encodedCutover) {
        private TerminalTarget {
            Objects.requireNonNull(source, "source");
            encodedPrepare = Objects.requireNonNull(
                encodedPrepare, "encodedPrepare").clone();
            encodedCutover = encodedCutover == null
                ? null
                : encodedCutover.clone();
        }

        @Override public byte[] encodedPrepare() {
            return encodedPrepare.clone();
        }

        @Override public byte[] encodedCutover() {
            return encodedCutover == null ? null : encodedCutover.clone();
        }
    }

    @FunctionalInterface
    interface RetentionScheduler {
        void schedule(Instant deadline, Runnable cleanup);
    }

    private static final class TargetAttempt {
        private final ZLinkCanonicalRelocationProtocol.Prepare prepare;
        private final ZLinkRelocationPayloadTransfer.Assembler assembler;
        private final RelayBoundary boundary = new RelayBoundary();
        private final CompletableFuture<ZLinkSpotRetireControl.StageRequest>
            request = new CompletableFuture<>();
        private final CompletableFuture<
            ZLinkAggregateRelocationCoordinator.Prepared> prepared =
                new CompletableFuture<>();
        private final CompletableFuture<Void> ready = new CompletableFuture<>();
        private CompletionStage<Void> publication;
        private CompletionStage<Void> readyPublication;
        private boolean fallbackArmed;
        private boolean committed;
        private volatile long committedNanos;
        private volatile byte[] receivedCutover;

        TargetAttempt(ZLinkCanonicalRelocationProtocol.Prepare prepare) {
            this.prepare = prepare;
            this.assembler = new ZLinkRelocationPayloadTransfer.Assembler(
                prepare.manifest());
        }

        ZLinkCanonicalRelocationProtocol.Prepare prepare() {
            return prepare;
        }

        ZLinkRelocationPayloadTransfer.Assembler assembler() {
            return assembler;
        }

        RelayBoundary boundary() {
            return boundary;
        }

        long committedNanos() {
            return committedNanos;
        }

        void committedNanos(long value) {
            committedNanos = value;
        }

        byte[] receivedCutover() {
            byte[] value = receivedCutover;
            return value == null ? null : value.clone();
        }

        void receivedCutover(byte[] value) {
            receivedCutover = value.clone();
        }

        CompletableFuture<ZLinkSpotRetireControl.StageRequest> request() {
            return request;
        }

        CompletableFuture<Void> ready() {
            return ready;
        }

        CompletableFuture<ZLinkAggregateRelocationCoordinator.Prepared>
            prepared() {
            return prepared;
        }

        CompletionStage<Void> publication() {
            return publication;
        }

        void publication(CompletionStage<Void> value) {
            publication = value;
        }

        CompletionStage<Void> readyPublication() {
            return readyPublication;
        }

        void readyPublication(CompletionStage<Void> value) {
            readyPublication = value;
        }

        synchronized boolean fallbackArmed() {
            return fallbackArmed;
        }

        synchronized void fallbackArmed(boolean value) {
            fallbackArmed = value;
        }

        synchronized boolean committed() {
            return committed;
        }

        synchronized void committed(boolean value) {
            committed = value;
        }
    }

}
