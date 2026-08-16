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

/** Owns the exact prepare/ready/data/cutover relocation attempt state. */
final class ZLinkCanonicalRelocationStateMachine
    implements ZLinkCanonicalRelocationTransitionOwner.StateMachine,
        ZLinkRelocationTransitionClient {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final Logger LOGGER = Logger.getLogger(
        ZLinkCanonicalRelocationStateMachine.class.getName());
    private static final String ACTOR_AUTHORITY_PREFIX = "zla1:a:";
    private static final int SCAN_PAGE_SIZE = 1000;
    private static final Duration CUTOVER_FALLBACK = Duration.ofSeconds(1);
    private static final Duration STORE_RETRY_DELAY = Duration.ofMillis(25);

    private final ZLinkInternalMeshNode node;
    private final String meshName;
    private final String entrySpotId;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkSpotRetireControl.TargetEndpoint target;
    private final RetentionScheduler retentionScheduler;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ConcurrentHashMap<Fence, SourceAttempt> sources =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Fence, TargetAttempt> targets =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Fence, RetryPrepared> retryPrepared =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Fence, TerminalTarget> terminalTargets =
        new ConcurrentHashMap<>();

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
        this.node = Objects.requireNonNull(node, "node");
        this.meshName = requireText(meshName, "meshName");
        this.entrySpotId = entrySpotId;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.target = Objects.requireNonNull(target, "target");
        this.retentionScheduler = Objects.requireNonNull(
            retentionScheduler, "retentionScheduler");
        localNodeRid = node.status().routingId();
        localNodeGeneration = node.status().lifecycleGeneration();
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
        return send(targetNodeRid,
            ZLinkCanonicalRelocationProtocol.encodeData(
                new ZLinkCanonicalRelocationProtocol.Data(
                    prepare.id(),
                    prepare.targetAttemptGeneration(),
                    prepare.coordinator(),
                    ZLinkCanonicalRelocationProtocol.SOURCE,
                    prepare.object(),
                    frozenRecord)));
    }

    @Override
    public CompletionStage<Void> publish(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout) {
        requireTimeout(timeout);
        SourceAttempt attempt = requireSource(Fence.from(fence), targetNodeRid);
        var prepare = attempt.prepare();
        var cutover = new ZLinkCanonicalRelocationProtocol.Cutover(
            prepare.id(),
            prepare.targetAttemptGeneration(),
            prepare.coordinator(),
            ZLinkCanonicalRelocationProtocol.SOURCE,
            prepare.object());
        Fence key = Fence.from(fence);
        return send(targetNodeRid,
                ZLinkCanonicalRelocationProtocol.encodeCutover(cutover))
            .whenComplete((ignored, failure) ->
                sources.remove(key, attempt));
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

    @Override
    public CompletionStage<Void> apply(
        RoutingId transportSource,
        int command,
        byte[] encoded) {
        try {
            return switch (command) {
                case ServiceWireConstants.COMMAND_RELOCATION_PREPARE ->
                    onPrepare(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodePrepare(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_READY ->
                    onReady(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeReady(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_DATA ->
                    onData(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeData(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_CUTOVER ->
                    onCutover(transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeCutover(encoded));
                default -> failed(new IllegalArgumentException(
                    "unsupported canonical relocation command"));
            };
        } catch (RuntimeException failure) {
            return failed(failure);
        }
    }

    private CompletionStage<Void> onPrepare(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
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
                return CompletableFuture.completedFuture(null);
            }
            return failed(new IllegalArgumentException(
                "terminal canonical relocation prepare differs"));
        }
        RetryPrepared foundRetry = retryPrepared.get(fence);
        if (foundRetry != null
            && !Instant.now().isBefore(
                foundRetry.prepared().stored().expiresAt())) {
            retryPrepared.remove(fence, foundRetry);
            foundRetry = null;
        }
        RetryPrepared retry = foundRetry;
        if (retry != null && !java.util.Arrays.equals(
                retry.encodedPrepare(),
                ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))) {
            return failed(new IllegalArgumentException(
                "retry canonical relocation prepare differs"));
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
            return publishReady(fence, attempt, transportSource);
        }

        reconstruct(prepare)
            .thenCompose(restore -> {
                attempt.request().complete(restore.request());
                return target.stage(restore.request())
                    .thenCompose(ignored -> retry == null
                        ? coordinator.prepareExistingRoot(
                            restore.authority(),
                            restore.root(),
                            prepare.root().reference(),
                            prepare.root().checksum(),
                            OPEN)
                        : CompletableFuture.completedFuture(retry.prepared()))
                    .thenAccept(attempt.prepared()::complete);
            })
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    attempt.ready().complete(null);
                } else {
                    targets.remove(fence, attempt);
                    attempt.ready().completeExceptionally(unwrap(failure));
                }
            });
        return publishReady(fence, attempt, transportSource);
    }

    private CompletionStage<Void> publishReady(
        Fence fence,
        TargetAttempt attempt,
        RoutingId source) {
        synchronized (attempt) {
            if (attempt.readyPublication() != null) {
                return attempt.readyPublication();
            }
            CompletionStage<Void> publication = attempt.ready()
                .thenCompose(ignored -> sendReady(source, attempt.prepare()))
                .thenRun(() -> {
                    RetryPrepared retry = retryPrepared.get(fence);
                    if (retry != null
                        && retry.prepared() == attempt.prepared().join()) {
                        retryPrepared.remove(fence, retry);
                    }
                    attempt.fallbackArmed(true);
                    scheduleCutoverFallback(fence, attempt);
                })
                .exceptionallyCompose(failure ->
                    rollbackReadySubmission(fence, attempt, unwrap(failure)));
            attempt.readyPublication(publication);
            return publication;
        }
    }

    private CompletionStage<Void> rollbackReadySubmission(
        Fence fence,
        TargetAttempt attempt,
        Throwable readyFailure) {
        if (attempt.fallbackArmed()) {
            return failed(readyFailure);
        }
        targets.remove(fence, attempt);
        ZLinkAggregateRelocationCoordinator.Prepared prepared =
            completedValue(attempt.prepared());
        if (prepared != null) {
            RetryPrepared retained = new RetryPrepared(
                ZLinkCanonicalRelocationProtocol.encodePrepare(
                    attempt.prepare()),
                prepared);
            if (retryPrepared.putIfAbsent(fence, retained) == null) {
                retentionScheduler.schedule(
                    prepared.stored().expiresAt(),
                    () -> retryPrepared.remove(fence, retained));
            }
        }
        ZLinkSpotRetireControl.StageRequest request =
            completedValue(attempt.request());
        CompletionStage<Void> rollback = request == null
            ? CompletableFuture.completedFuture(null)
            : target.abort(request);
        return rollback
            .handle((ignored, targetFailure) -> {
                if (targetFailure != null) {
                    readyFailure.addSuppressed(unwrap(targetFailure));
                }
                throw new CompletionException(readyFailure);
            });
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
            if (terminal != null
                && terminal.source().equals(transportSource)
                && java.util.Arrays.equals(
                    terminal.encodedCutover(),
                    ZLinkCanonicalRelocationProtocol.encodeCutover(cutover))) {
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
        return publishTarget(fence, attempt, false);
    }

    private void scheduleCutoverFallback(
        Fence fence, TargetAttempt attempt) {
        CompletableFuture.delayedExecutor(
                CUTOVER_FALLBACK.toMillis(), TimeUnit.MILLISECONDS)
            .execute(() -> publishTarget(fence, attempt, true)
                .exceptionally(ignored -> null));
    }

    private CompletionStage<Void> publishTarget(
        Fence fence,
        TargetAttempt attempt,
        boolean fallback) {
        CompletionStage<Void> publication;
        synchronized (attempt) {
            if (attempt.publication() != null) {
                return attempt.publication();
            }
            publication = attempt.prepared()
                .thenCompose(this::commitUntilRestoreExpiry)
                .thenApply(published -> {
                    attempt.committed(true);
                    return published;
                })
                .thenCompose(ignored -> attempt.request())
                .thenCompose(target::publish);
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
        return publication;
    }

    private void retainTerminalTarget(Fence fence, TargetAttempt attempt) {
        ZLinkAggregateRelocationCoordinator.Prepared prepared =
            attempt.prepared().join();
        var prepare = attempt.prepare();
        var cutover = new ZLinkCanonicalRelocationProtocol.Cutover(
            prepare.id(),
            prepare.targetAttemptGeneration(),
            prepare.coordinator(),
            ZLinkCanonicalRelocationProtocol.SOURCE,
            prepare.object());
        TerminalTarget terminal = new TerminalTarget(
            prepare.sourceNodeRid(),
            ZLinkCanonicalRelocationProtocol.encodePrepare(prepare),
            ZLinkCanonicalRelocationProtocol.encodeCutover(cutover));
        TerminalTarget current = terminalTargets.putIfAbsent(fence, terminal);
        if (current == null) {
            retentionScheduler.schedule(
                prepared.stored().expiresAt(),
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
        if (!Instant.now().isBefore(prepared.stored().expiresAt())) {
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
                || !Instant.now().isBefore(prepared.stored().expiresAt())) {
                result.completeExceptionally(cause);
                return;
            }
            long remainingMillis = Math.max(1L, Duration.between(
                Instant.now(), prepared.stored().expiresAt()).toMillis());
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
        CompletionStage<ZLinkAggregateRelocationCoordinator.Root> root =
            coordinator.readRoot(
                request.relocationReference(),
                request.relocationChecksum(),
                OPEN);
        return authority.thenCombine(root, (read, stored) -> {
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
            var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(
                stored.payload());
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
                new ZLinkCanonicalRelocationProtocol.Root(
                    request.relocationReference(),
                    request.relocationChecksum()),
                envelope.applicationVersion());
        });
    }

    private CompletionStage<TargetRestore> reconstruct(
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        if (prepare.root() == null) {
            return failed(new IllegalArgumentException(
                "canonical relocation root is required"));
        }
        CompletionStage<ZLinkAggregateRelocationCoordinator.Root> root =
            coordinator.readRoot(
                prepare.root().reference(), prepare.root().checksum(), OPEN);
        CompletionStage<List<ZLinkAuthorityEntry>> inventory =
            prepare.object().kind() == 1
                ? readStandaloneActor(prepare)
                : readUserSpotInventory(prepare);
        return root.thenCombine(inventory, (stored, entries) -> {
            ZLinkSpotRetireControl.TargetProfile profile =
                Objects.requireNonNull(
                    target.applyTargetProfile(stageRequest(
                        prepare, stored.payload(), entries),
                        localNodeGeneration),
                    "canonical target profile returned null");
            ZLinkSpotRetireControl.StageRequest request = profile.request();
            return new TargetRestore(
                request,
                authorityRequest(
                    prepare, stored.payload(), entries, profile),
                stored);
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
        ZLinkActorAuthorityPayloadCodec actorCodec =
            new ZLinkActorAuthorityPayloadCodec();
        for (ZLinkAuthorityEntry entry : entries) {
            ZLinkAuthoritySnapshot snapshot = entry.snapshot();
            byte[] authorityPayload = snapshot.payload();
            byte[] membershipMutation = new byte[0];
            if (standalone) {
                var actor = actorCodec.decode(snapshot.payload())
                    .orElseThrow(() -> new IllegalStateException(
                        "standalone Actor authority payload is invalid"));
                authorityPayload = actorCodec.encode(
                    ZLinkActorAuthorityPayloadCodec.State.READY,
                    actor.stableType(),
                    actor.actorId(),
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
            prepare.targetAttemptGeneration(),
            participants,
            root,
            new ZLinkMeshNodeDescriptorKey(meshName, localNodeRid),
            localNodeGeneration,
            capacity,
            new ZLinkLocationOwnerToken(
                request.targetOwnerId(),
                request.targetOwnerLeaseGeneration()));
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
                "target relocation inventory differs from root");
        }
        Map<Long, Boolean> restore = new HashMap<>();
        for (var state : envelope.applicationStates()) {
            restore.put(state.participantId(), state.hasState());
        }
        List<ZLinkSpotRetireControl.ParticipantFence> participants =
            new ArrayList<>(entries.size());
        var spotCodec = new ZLinkServiceAuthorityPayloadCodec();
        var actorCodec = new ZLinkActorAuthorityPayloadCodec();
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
                var decoded = actorCodec.decode(snapshot.payload())
                    .orElseThrow(() -> new IllegalStateException(
                        "Actor authority payload is invalid"));
                objectId = decoded.actorId();
                objectType = decoded.stableType();
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
        if (primarySnapshot == null
            || !primarySnapshot.storeVersion().equals(
                prepare.coordinator().expectedAuthorityStoreVersion())) {
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
            prepare.root().reference(),
            prepare.root().checksum(),
            participants,
            List.of());
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
     * duplicate PREPARE is idempotent on the target. PREPARE is a one-way
     * submission that can be lost while symmetric manual duplicate
     * connections converge (spec 07 §518), so waiting the whole deadline on
     * one submission turns a lost frame into a silent Join stall. Resend the
     * idempotent PREPARE each bounded slice until relay readiness or the
     * remaining deadline is spent.
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
        CompletionStage<Void> submitted = submitFirst && !attempt.ready().isDone()
            ? send(targetNodeRid,
                ZLinkCanonicalRelocationProtocol.encodePrepare(attempt.prepare()))
            : CompletableFuture.completedFuture(null);
        Duration slice = Duration.ofNanos(
            Math.min(remainingNanos, TimeUnit.SECONDS.toNanos(1)));
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
        AtomicInteger activeWaiters) {
        SourceAttempt(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkCanonicalRelocationProtocol.Prepare prepare) {
            this(
                request,
                prepare,
                new CompletableFuture<>(),
                new AtomicInteger());
        }
    }

    private record TargetRestore(
        ZLinkSpotRetireControl.StageRequest request,
        ZLinkAggregateRelocationCoordinator.Request authority,
        ZLinkAggregateRelocationCoordinator.Root root) {
    }

    private record RetryPrepared(
        byte[] encodedPrepare,
        ZLinkAggregateRelocationCoordinator.Prepared prepared) {
        private RetryPrepared {
            encodedPrepare = encodedPrepare.clone();
            Objects.requireNonNull(prepared, "prepared");
        }

        @Override public byte[] encodedPrepare() {
            return encodedPrepare.clone();
        }
    }

    private record TerminalTarget(
        RoutingId source,
        byte[] encodedPrepare,
        byte[] encodedCutover) {
        private TerminalTarget {
            Objects.requireNonNull(source, "source");
            encodedPrepare = Objects.requireNonNull(
                encodedPrepare, "encodedPrepare").clone();
            encodedCutover = Objects.requireNonNull(
                encodedCutover, "encodedCutover").clone();
        }

        @Override public byte[] encodedPrepare() {
            return encodedPrepare.clone();
        }

        @Override public byte[] encodedCutover() {
            return encodedCutover.clone();
        }
    }

    @FunctionalInterface
    interface RetentionScheduler {
        void schedule(Instant deadline, Runnable cleanup);
    }

    private static final class TargetAttempt {
        private final ZLinkCanonicalRelocationProtocol.Prepare prepare;
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

        TargetAttempt(ZLinkCanonicalRelocationProtocol.Prepare prepare) {
            this.prepare = prepare;
        }

        ZLinkCanonicalRelocationProtocol.Prepare prepare() {
            return prepare;
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
