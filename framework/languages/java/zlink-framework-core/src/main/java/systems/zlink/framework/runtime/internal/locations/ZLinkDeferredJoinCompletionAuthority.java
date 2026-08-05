package systems.zlink.framework.runtime.internal.locations;

import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.spots.ZLinkCanonicalActorRelocationEnvelope;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * Stores deferred Actor Join completion progress inside the Actor's canonical
 * relocation root. Every cursor update writes an immutable successor before
 * the Location authority CAS, then removes the root no longer referenced.
 */
public final class ZLinkDeferredJoinCompletionAuthority {
    private static final Duration RETENTION = Duration.ofHours(24);
    private static final ZLinkStoreCancellation NEVER = () -> false;
    private static final String PACKET = "ZLinkActorJoinAccepted";
    private static final String CONTENT_TYPE = "application/octet-stream";
    private static final String RELOCATION_CONTENT_TYPE =
        "application/vnd.zlink.deferred-join-relocation-v1";
    private static final int RELOCATION_COMPLETION_MAGIC = 0x5A4C444A;
    private static final int RELOCATION_COMPLETION_VERSION = 1;

    private final ZLinkLocationRepository authority;
    private final ZLinkRelocationStore relocation;

    public ZLinkDeferredJoinCompletionAuthority(
        ZLinkLocationRepository authority,
        ZLinkRelocationStore relocation) {
        this.authority = Objects.requireNonNull(authority, "authority");
        this.relocation = Objects.requireNonNull(relocation, "relocation");
    }

    public CompletionStage<Published> prepare(
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        byte[] rawReply) {
        return read(actor.actorId()).thenCompose(current -> {
            validateActor(current.root(), actor);
            var existing = find(
                current.root().terminalCompletions(), operationId);
            if (existing != null) {
                validateCompletion(existing, actor, rawReply);
                return CompletableFuture.completedFuture(
                    published(current, existing));
            }
            long participantId = onlyParticipant(current.root());
            long sequence = current.root().terminalCompletions().stream()
                .filter(value -> value.participantId() == participantId)
                .mapToLong(
                    ZLinkServiceRelocationEnvelopeCodec.Completion::sequence)
                .reduce(0L, (left, right) ->
                    Long.compareUnsigned(left, right) >= 0 ? left : right);
            if (sequence == -1L) {
                return CompletableFuture.failedFuture(
                    new CanonicalRootUnavailableException(
                        "deferred Join completion sequence is exhausted"));
            }
            var completion = new ZLinkServiceRelocationEnvelopeCodec.Completion(
                operationId.high(),
                operationId.low(),
                current.publication().sourceOwnerId(),
                current.publication().sourceOwnerLeaseGeneration(),
                current.publication().sourceNodeRid().toString(),
                current.publication().sourceNodeGeneration(),
                participantId,
                sequence + 1,
                0,
                0,
                1,
                new ZLinkServiceRelocationEnvelopeCodec.Payload(
                    PACKET,
                    CONTENT_TYPE,
                    rawReply == null ? new byte[0] : rawReply));
            var successor =
                ZLinkServiceRelocationEnvelopeCodec.putTerminalCompletion(
                    current.root(), completion);
            return publish(current, successor)
                .thenApply(next -> published(next, completion));
        });
    }

    /**
     * Prepares a direct Actor Join as one canonical relocation aggregate.
     * The returned fence is committed by the target before it exposes the
     * restored Actor or invokes the application callback.
     */
    public CompletionStage<PreparedPublication> prepareRelocation(
        UUID relocationId,
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        String actorType,
        String targetSpotId,
        RoutingId targetNodeRid,
        boolean restoreSnapshot,
        byte[] applicationState,
        List<ZLinkAsyncSerialQueue.QueuedRecord> acceptedJournal,
        byte[] rawReply,
        byte[] sessionRouteCommand44) {
        String actorKey = ZLinkAuthorityKeyCodec.actor(actor.actorId());
        String spotKey = ZLinkAuthorityKeyCodec.spot(targetSpotId);
        return authority.read(actorKey, NEVER).thenCompose(actorRead -> {
            if (!(actorRead instanceof ZLinkAuthoritySnapshot actorSnapshot)) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Actor authority is missing for direct Join relocation"));
            }
            var source = new ZLinkActorAuthorityPayloadCodec()
                .decode(actorSnapshot.payload())
                .orElseThrow(() -> new IllegalStateException(
                    "Actor authority payload is invalid"));
            if (!source.actorId().equals(actor.actorId())
                || !source.stableType().equals(actorType)
                || actorSnapshot.objectGeneration() != actor.generation()) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Actor authority changed before direct Join relocation"));
            }
            return authority.read(spotKey, NEVER).thenCompose(spotRead -> {
                if (!(spotRead instanceof ZLinkAuthoritySnapshot spotSnapshot)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "target Spot authority is missing"));
                }
                var target = new ZLinkServiceAuthorityPayloadCodec()
                    .decode(spotSnapshot.payload())
                    .orElseThrow(() -> new IllegalStateException(
                        "target Spot authority payload is invalid"));
                if (target.state()
                        != ZLinkServiceAuthorityPayloadCodec.State.READY
                    || target.kind()
                        != ZLinkServiceAuthorityPayloadCodec.Kind.USER
                    || !target.spotId().equals(targetSpotId)
                    || !target.nodeRid().equals(targetNodeRid)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "target Spot authority changed before relocation"));
                }
                List<ZLinkAsyncSerialQueue.QueuedRecord> journal =
                    List.copyOf(acceptedJournal);
                byte[] initial =
                    ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId,
                        actor.actorId(),
                        actor.generation(),
                        actorSnapshot.authorityOwnerGeneration(),
                        restoreSnapshot,
                        applicationState,
                        journal);
                var envelope =
                    ZLinkServiceRelocationEnvelopeCodec.decode(initial);
                long sequence = journal.isEmpty()
                    ? 1L
                    : journal.getLast().sequence() + 1L;
                var completion =
                    new ZLinkServiceRelocationEnvelopeCodec.Completion(
                        operationId.high(),
                        operationId.low(),
                        actorSnapshot.ownerId(),
                        actorSnapshot.ownerLeaseGeneration(),
                        source.nodeRid().toString(),
                        source.nodeGeneration(),
                        1,
                        sequence,
                        0,
                        0,
                        1,
                        new ZLinkServiceRelocationEnvelopeCodec.Payload(
                            PACKET,
                            RELOCATION_CONTENT_TYPE,
                            encodeRelocationCompletion(
                                rawReply,
                                sessionRouteCommand44)));
                byte[] root =
                    ZLinkServiceRelocationEnvelopeCodec.encodeSuccessor(
                        envelope,
                        envelope.participantProgress(),
                        List.of(completion));
                byte[] targetAuthority =
                    new ZLinkActorAuthorityPayloadCodec().encode(
                        ZLinkActorAuthorityPayloadCodec.State.READY,
                        actorType,
                        actor.actorId(),
                        targetSpotId,
                        spotSnapshot.objectGeneration(),
                        2,
                        spotSnapshot.ownerId(),
                        spotSnapshot.ownerLeaseGeneration(),
                        target.meshName(),
                        target.nodeRid(),
                        target.nodeGeneration());
                var participant =
                    new ZLinkAggregateRelocationCoordinator.Participant(
                        actorKey,
                        ZLinkPlacementObjectKind.ACTOR,
                        actorSnapshot.objectGeneration(),
                        actorSnapshot.authorityOwnerGeneration(),
                        actorSnapshot.storeVersion(),
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        targetAuthority,
                        new byte[0]);
                var coordinator = new ZLinkAggregateRelocationCoordinator(
                    authority, relocation);
                return authority.listMeshNodes(
                        target.meshName(),
                        new ZLinkPageRequest(1_000, null))
                    .thenCompose(page -> {
                        var descriptor = page.items().stream()
                            .filter(candidate ->
                                candidate.rid().equals(targetNodeRid))
                            .findFirst()
                            .orElseThrow(() -> new IllegalStateException(
                                "target MeshNode descriptor is missing"));
                        if (!descriptor.ownerId().equals(
                                spotSnapshot.ownerId())
                            || descriptor.leaseGeneration()
                                != spotSnapshot.ownerLeaseGeneration()
                            || descriptor.lifecycleGeneration()
                                != spotSnapshot.allocation()
                                    .descriptorLifecycleGeneration()) {
                            return CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "target Spot and MeshNode owner fences differ"));
                        }
                        var request =
                            new ZLinkAggregateRelocationCoordinator.Request(
                                relocationId,
                                1,
                                List.of(participant),
                                root,
                                new ZLinkMeshNodeDescriptorKey(
                                    descriptor.meshName(),
                                    descriptor.rid()),
                                descriptor.lifecycleGeneration(),
                                ZLinkPlacementCapacityBundle.actor(1),
                                new ZLinkLocationOwnerToken(
                                    descriptor.ownerId(),
                                    descriptor.leaseGeneration()));
                        return coordinator.prepare(request, NEVER)
                            .thenApply(prepared -> new PreparedPublication(
                                new Published(
                                    prepared.stored().reference(),
                                    prepared.stored().checksumCrc32c(),
                                    1,
                                    operationId,
                                    actor.actorId(),
                                    actor.generation(),
                                    rawReply == null
                                        ? new byte[0] : rawReply),
                                prepared.fence()));
                    });
            });
        });
    }

    public CompletionStage<Long> commitPrepared(
        UUID aggregateId,
        long aggregateGeneration,
        ZLinkBackendActorRef actor) {
        Objects.requireNonNull(actor, "actor");
        return authority.commitAggregate(
                new ZLinkAggregateFence(
                    aggregateId,
                    aggregateGeneration),
                NEVER)
            .thenCompose(result ->
                result == ZLinkAggregateCommitResult.COMMITTED
                    || result == ZLinkAggregateCommitResult.ALREADY_COMMITTED
                    ? readCommittedOwnerGeneration(
                        aggregateId,
                        aggregateGeneration,
                        actor)
                    : CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "direct Join relocation aggregate commit failed: "
                                + result)));
    }

    private CompletionStage<Long> readCommittedOwnerGeneration(
        UUID aggregateId,
        long aggregateGeneration,
        ZLinkBackendActorRef actor) {
        return authority.read(
                ZLinkAuthorityKeyCodec.actor(actor.actorId()),
                NEVER)
            .thenCompose(result -> {
                if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                    || snapshot.objectGeneration() != actor.generation()) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "direct Join Actor authority is missing after commit"));
                }
                var publication =
                    ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                        snapshot.payload());
                if (publication == null
                    || !publication.aggregateId().equals(aggregateId)
                    || publication.aggregateGeneration()
                        != aggregateGeneration
                    || !publication.targetOwnerId().equals(snapshot.ownerId())
                    || publication.targetOwnerLeaseGeneration()
                        != snapshot.ownerLeaseGeneration()
                    || !publication.targetNodeRid().equals(
                        snapshot.allocation().descriptor().rid())
                    || publication.targetNodeGeneration()
                        != snapshot.allocation()
                            .descriptorLifecycleGeneration()) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "direct Join canonical owner fence differs after commit"));
                }
                var payload = new ZLinkActorAuthorityPayloadCodec()
                    .decode(
                        publication.applicationPayload())
                    .orElse(null);
                if (payload == null
                    || !payload.actorId().equals(actor.actorId())
                    || !payload.ownerId().equals(snapshot.ownerId())
                    || payload.ownerLeaseGeneration()
                        != snapshot.ownerLeaseGeneration()
                    || !payload.meshName().equals(
                        snapshot.allocation().descriptor().meshName())
                    || !payload.nodeRid().equals(
                        snapshot.allocation().descriptor().rid())
                    || payload.nodeGeneration()
                        != snapshot.allocation()
                            .descriptorLifecycleGeneration()
                    || snapshot.allocation().objectKind()
                        != ZLinkPlacementObjectKind.ACTOR) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "direct Join Actor authority owner differs after commit"));
                }
                return load(
                        publication.reference(),
                        publication.checksumCrc32c())
                    .thenApply(loaded -> {
                        var root = loaded.root();
                        if (root.relocationHigh()
                                != aggregateId.getMostSignificantBits()
                            || root.relocationLow()
                                != aggregateId.getLeastSignificantBits()) {
                            throw new IllegalStateException(
                                "direct Join canonical root identity differs after commit");
                        }
                        return snapshot.authorityOwnerGeneration();
                    });
            });
    }

    /**
     * Waits until the aggregate commit has transferred the Actor authority to
     * the target owner. The source uses this infrastructure-side observation
     * before releasing its local Actor resources.
     */
    public CompletionStage<Void> awaitTargetCommit(
        UUID aggregateId,
        long aggregateGeneration,
        ZLinkBackendActorRef actor,
        Duration timeout) {
        Objects.requireNonNull(aggregateId, "aggregateId");
        Objects.requireNonNull(actor, "actor");
        Objects.requireNonNull(timeout, "timeout");
        if (aggregateGeneration <= 0
            || timeout.isZero()
            || timeout.isNegative()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "deferred Join target-commit wait is invalid"));
        }
        return await(
            () -> authority.read(
                    ZLinkAuthorityKeyCodec.actor(actor.actorId()),
                    NEVER)
                .thenApply(result -> {
                    if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                        || snapshot.objectGeneration() != actor.generation()) {
                        return false;
                    }
                    var publication =
                        ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                            snapshot.payload());
                    return publication != null
                        && publication.aggregateId().equals(aggregateId)
                        && publication.aggregateGeneration()
                            == aggregateGeneration
                        && snapshot.ownerId().equals(
                            publication.targetOwnerId())
                        && snapshot.ownerLeaseGeneration()
                            == publication.targetOwnerLeaseGeneration();
                }),
            timeout,
            "deferred Join target commit");
    }

    /**
     * Waits until the target has completed the durable Join completion
     * callback.  Source cleanup is allowed only after this point; the target
     * callback itself must not wait for source cleanup.
     */
    public CompletionStage<Void> awaitTargetCompletion(
        UUID aggregateId,
        long aggregateGeneration,
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        Duration timeout) {
        Objects.requireNonNull(aggregateId, "aggregateId");
        Objects.requireNonNull(operationId, "operationId");
        Objects.requireNonNull(actor, "actor");
        Objects.requireNonNull(timeout, "timeout");
        if (aggregateGeneration <= 0
            || timeout.isZero()
            || timeout.isNegative()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "deferred Join target-completion wait is invalid"));
        }
        return await(
            () -> authority.read(
                    ZLinkAuthorityKeyCodec.actor(actor.actorId()),
                    NEVER)
                .thenCompose(result -> {
                    if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                        || snapshot.objectGeneration() != actor.generation()) {
                        return CompletableFuture.completedFuture(false);
                    }
                    var publication =
                        ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                            snapshot.payload());
                    if (publication == null
                        || !publication.aggregateId().equals(aggregateId)
                        || publication.aggregateGeneration()
                            != aggregateGeneration
                        || !snapshot.ownerId().equals(
                            publication.targetOwnerId())
                        || snapshot.ownerLeaseGeneration()
                            != publication.targetOwnerLeaseGeneration()) {
                        return CompletableFuture.completedFuture(false);
                    }
                    return load(
                            publication.reference(),
                            publication.checksumCrc32c())
                        .thenApply(loaded -> {
                            var completion = find(
                                loaded.root().terminalCompletions(),
                                operationId);
                            return completion != null
                                && completion.deliveryState() >= 3;
                        });
                }),
            timeout,
            "deferred Join target completion");
    }

    /**
     * Waits for the source-cleanup CAS that closes the relocation admission
     * gate before target route activation and application callbacks proceed.
     */
    public CompletionStage<Void> awaitSourceCleanup(
        UUID aggregateId,
        long aggregateGeneration,
        ZLinkBackendActorRef actor,
        Duration timeout) {
        Objects.requireNonNull(aggregateId, "aggregateId");
        Objects.requireNonNull(actor, "actor");
        Objects.requireNonNull(timeout, "timeout");
        if (aggregateGeneration <= 0
            || timeout.isZero()
            || timeout.isNegative()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "deferred Join source-cleanup wait is invalid"));
        }
        return await(
            () -> authority.read(
                    ZLinkAuthorityKeyCodec.actor(actor.actorId()),
                    NEVER)
                .thenApply(result -> {
                    if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                        || snapshot.objectGeneration() != actor.generation()) {
                        return false;
                    }
                    var publication =
                        ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                            snapshot.payload());
                    return publication != null
                        && publication.aggregateId().equals(aggregateId)
                        && publication.aggregateGeneration()
                            == aggregateGeneration
                        && publication.sourceCleanupCompleted()
                        && snapshot.ownerId().equals(
                            publication.targetOwnerId())
                        && snapshot.ownerLeaseGeneration()
                            == publication.targetOwnerLeaseGeneration();
                }),
            timeout,
            "deferred Join source cleanup");
    }

    /**
     * Publishes the source-cleanup phase without releasing the canonical root.
     * The completion root must remain addressable until the target callback has
     * advanced its delivery cursor to Delivered.
     */
    public CompletionStage<Void> markSourceCleanup(
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor) {
        Objects.requireNonNull(operationId, "operationId");
        Objects.requireNonNull(actor, "actor");
        return read(actor.actorId()).thenCompose(current -> {
            var completion = find(
                current.root().terminalCompletions(), operationId);
            if (completion == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "deferred Join source-cleanup operation is missing"));
            }
            if (current.publication().sourceCleanupCompleted()) {
                return CompletableFuture.completedFuture(null);
            }
            var stored = new ZLinkRelocationStored(
                current.publication().reference(),
                current.publication().checksumCrc32c(),
                current.snapshot().storeNow(),
                current.snapshot().storeNow());
            byte[] completed =
                ZLinkCanonicalRelocationAuthorityStateCodec
                    .completeSourceCleanup(
                        current.snapshot().payload(),
                        stored,
                        current.root());
            return authority.compareExchange(
                    current.authorityKey(),
                    new ZLinkAuthorityExpectFound(
                        current.snapshot().storeVersion()),
                    new ZLinkAuthorityPut(
                        completed,
                        ZLinkAuthorityGenerationTransition.PRESERVE,
                        Optional.empty(),
                        Optional.empty()),
                    NEVER)
                .thenCompose(result -> result instanceof ZLinkAuthorityStored
                    ? CompletableFuture.completedFuture(null)
                    : CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "deferred Join source-cleanup CAS conflicted")));
        });
    }

    public CompletionStage<Void> abortPrepared(
        UUID aggregateId,
        long aggregateGeneration,
        String reference) {
        return authority.abortAggregate(
                new ZLinkAggregateFence(
                    aggregateId,
                    aggregateGeneration),
                NEVER)
            .thenCompose(result -> {
                if (result != ZLinkAggregateAbortResult.ABORTED
                    && result != ZLinkAggregateAbortResult.ALREADY_ABORTED) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "direct Join relocation aggregate abort failed: "
                                + result));
                }
                return ZLinkRelocationTreeStore.delete(
                    relocation,
                    reference,
                    NEVER);
            });
    }

    public CompletionStage<PreparedRoot> loadPrepared(
        String reference,
        long checksumCrc32c,
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        UUID relocationId,
        boolean restoreSnapshot) {
        return load(reference, checksumCrc32c).thenApply(loaded -> {
            var root = loaded.root();
            validateActor(root, actor);
            if (root.relocationHigh()
                    != relocationId.getMostSignificantBits()
                || root.relocationLow()
                    != relocationId.getLeastSignificantBits()
                || root.applicationStates().size() != 1
                || root.participantProgress().size() != 1
                || !root.timerRegistrations().isEmpty()
                || !root.pendingTimerTicks().isEmpty()) {
                throw new IllegalStateException(
                    "direct Join canonical relocation root is invalid");
            }
            var state = root.applicationStates().getFirst();
            if (state.participantId() != 1
                || state.hasState() != restoreSnapshot) {
                throw new IllegalStateException(
                    "direct Join relocation state policy differs");
            }
            var completion = find(root.terminalCompletions(), operationId);
            if (completion == null) {
                throw new IllegalStateException(
                    "direct Join relocation root has no completion record");
            }
            List<ZLinkAsyncSerialQueue.QueuedRecord> journal =
                root.journal().stream()
                    .map(value -> {
                        if (value.participantId() != 1) {
                            throw new IllegalStateException(
                                "direct Join journal references another participant");
                        }
                        return new ZLinkAsyncSerialQueue.QueuedRecord(
                            value.sequence(),
                            value.rawEntry());
                    })
                    .toList();
            return new PreparedRoot(
                state.payload(),
                journal,
                decodeRelocationCompletion(completion).sessionRouteCommand44());
        });
    }

    public CompletionStage<Published> advance(
        Published expected,
        ZLinkBackendActorRef actor,
        int cursor) {
        if (cursor < 1 || cursor > 3
            || cursor > expected.cursor() + 1) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "invalid deferred Join completion cursor"));
        }
        return read(actor.actorId()).thenCompose(current -> {
            validateActor(current.root(), actor);
            var completion = find(
                current.root().terminalCompletions(),
                expected.operationId());
            if (completion == null) {
                return CompletableFuture.failedFuture(
                    new CanonicalRootUnavailableException(
                        "Actor authority no longer references the deferred Join completion"));
            }
            validateCompletion(
                completion, actor, expected.rawReply());
            if (completion.deliveryState() >= cursor) {
                return CompletableFuture.completedFuture(
                    published(current, completion));
            }
            var updated =
                new ZLinkServiceRelocationEnvelopeCodec.Completion(
                    completion.operationHigh(),
                    completion.operationLow(),
                    completion.sourceOwnerId(),
                    completion.sourceOwnerLeaseGeneration(),
                    completion.sourceNodeRid(),
                    completion.sourceNodeGeneration(),
                    completion.participantId(),
                    completion.sequence(),
                    completion.terminalResult(),
                    completion.failureCode(),
                    cursor,
                    completion.payload());
            var successor =
                ZLinkServiceRelocationEnvelopeCodec.putTerminalCompletion(
                    current.root(), updated);
            return publish(current, successor)
                .thenApply(next -> published(next, updated));
        });
    }

    public CompletionStage<Void> release(
        Published delivered,
        ZLinkBackendActorRef actor) {
        if (delivered.cursor() != 3) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "deferred Join completion must be Delivered before release"));
        }
        return authority.read(
                ZLinkAuthorityKeyCodec.actor(actor.actorId()),
                NEVER)
            .thenCompose(result -> {
            if (!(result instanceof ZLinkAuthoritySnapshot snapshot)) {
                return CompletableFuture.completedFuture(null);
            }
            var publication =
                ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                    snapshot.payload());
            if (publication == null) {
                return CompletableFuture.completedFuture(null);
            }
            return load(
                    publication.reference(),
                    publication.checksumCrc32c())
                .thenCompose(loaded -> release(
                    delivered,
                    actor,
                    new Current(
                        ZLinkAuthorityKeyCodec.actor(actor.actorId()),
                        snapshot,
                        publication,
                        loaded.root(),
                        loaded.inventoryDigest())));
        });
    }

    private CompletionStage<Void> release(
        Published delivered,
        ZLinkBackendActorRef actor,
        Current current) {
            validateActor(current.root(), actor);
            var completion = find(
                current.root().terminalCompletions(),
                delivered.operationId());
            if (completion == null) {
                return CompletableFuture.completedFuture(null);
            }
            if (completion.deliveryState() != 3) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Actor authority completion is not Delivered"));
            }
            if (!current.publication().sourceCleanupCompleted()) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "deferred Join completion cannot be released before source cleanup"));
            }
            return authority.compareExchange(
                    current.authorityKey(),
                    new ZLinkAuthorityExpectFound(
                        current.snapshot().storeVersion()),
                    new ZLinkAuthorityPut(
                        current.publication().applicationPayload(),
                        ZLinkAuthorityGenerationTransition.PRESERVE,
                        Optional.empty(),
                        Optional.empty()),
                    NEVER)
                .thenCompose(result ->
                    result instanceof ZLinkAuthorityStored
                        ? ZLinkRelocationTreeStore.delete(
                                relocation,
                                current.publication().reference(),
                                NEVER)
                        : CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "deferred Join completion release CAS conflicted")));
    }

    public CompletionStage<Void> completeSourceCleanupAndRelease(
        Published delivered,
        ZLinkBackendActorRef actor) {
        return read(actor.actorId()).thenCompose(current -> {
            var completion = find(
                current.root().terminalCompletions(),
                delivered.operationId());
            if (completion == null || completion.deliveryState() != 3) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "deferred Join completion is not durably Delivered"));
            }
            if (current.publication().sourceCleanupCompleted()) {
                return release(delivered, actor);
            }
            var stored = new ZLinkRelocationStored(
                current.publication().reference(),
                current.publication().checksumCrc32c(),
                current.snapshot().storeNow(),
                current.snapshot().storeNow());
            byte[] completed =
                ZLinkCanonicalRelocationAuthorityStateCodec
                    .completeSourceCleanup(
                        current.snapshot().payload(),
                        stored,
                        current.root());
            return authority.compareExchange(
                    current.authorityKey(),
                    new ZLinkAuthorityExpectFound(
                        current.snapshot().storeVersion()),
                    new ZLinkAuthorityPut(
                        completed,
                        ZLinkAuthorityGenerationTransition.PRESERVE,
                        Optional.empty(),
                        Optional.empty()),
                    NEVER)
                .thenCompose(result -> {
                    if (!(result instanceof ZLinkAuthorityStored)) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "deferred Join source-cleanup CAS conflicted"));
                    }
                    return release(delivered, actor);
                });
        });
    }

    private CompletionStage<Void> await(
        Supplier<CompletionStage<Boolean>> condition,
        Duration timeout,
        String description) {
        long timeoutNanos;
        try {
            timeoutNanos = timeout.toNanos();
        } catch (ArithmeticException overflow) {
            timeoutNanos = Long.MAX_VALUE;
        }
        long started = System.nanoTime();
        CompletableFuture<Void> result = new CompletableFuture<>();
        poll(condition, started, timeoutNanos, description, result);
        return result;
    }

    private void poll(
        Supplier<CompletionStage<Boolean>> condition,
        long started,
        long timeoutNanos,
        String description,
        CompletableFuture<Void> result) {
        if (result.isDone()) {
            return;
        }
        long elapsed = System.nanoTime() - started;
        if (elapsed >= timeoutNanos) {
            result.completeExceptionally(new IllegalStateException(
                description + " did not complete before timeout"));
            return;
        }
        CompletionStage<Boolean> check;
        try {
            check = Objects.requireNonNull(
                condition.get(), "deferred Join wait condition");
        } catch (Throwable error) {
            result.completeExceptionally(error);
            return;
        }
        check.whenComplete((ready, error) -> {
            if (result.isDone()) {
                return;
            }
            if (error != null) {
                result.completeExceptionally(error);
                return;
            }
            if (Boolean.TRUE.equals(ready)) {
                result.complete(null);
                return;
            }
            long remaining = timeoutNanos - (System.nanoTime() - started);
            if (remaining <= 0) {
                result.completeExceptionally(new IllegalStateException(
                    description + " did not complete before timeout"));
                return;
            }
            long delayMillis = Math.max(
                1L,
                Math.min(10L, TimeUnit.NANOSECONDS.toMillis(remaining)));
            CompletableFuture.delayedExecutor(
                    delayMillis,
                    TimeUnit.MILLISECONDS)
                .execute(() -> poll(
                    condition,
                    started,
                    timeoutNanos,
                    description,
                    result));
        });
    }

    public CompletionStage<Published> restore(
        String reference,
        long checksumCrc32c,
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor) {
        return load(reference, checksumCrc32c).thenApply(loaded -> {
            var root = loaded.root();
            if (root.object().kind() != 1
                || !root.object().objectId().equals(actor.actorId())
                || root.object().objectGeneration() != actor.generation()) {
                throw new IllegalStateException(
                    "deferred Join manifest has a stale Actor generation");
            }
            var completion = find(root.terminalCompletions(), operationId);
            if (completion == null) {
                throw new IllegalStateException(
                    "deferred Join manifest does not contain the operation");
            }
            return new Published(
                reference,
                checksumCrc32c,
                completion.deliveryState(),
                new ZLinkActorJoinOperationId(
                    completion.operationHigh(),
                    completion.operationLow()),
                root.object().objectId(),
                root.object().objectGeneration(),
                completion.payload() == null
                    ? new byte[0]
                    : RELOCATION_CONTENT_TYPE.equals(
                        completion.payload().contentType())
                        ? decodeRelocationCompletion(completion).rawReply()
                        : completion.payload().bytes());
        });
    }

    public CompletionStage<Published> recover(
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor) {
        return read(actor.actorId()).thenApply(current -> {
            validateActor(current.root(), actor);
            var completion = find(
                current.root().terminalCompletions(), operationId);
            if (completion == null) {
                throw new IllegalStateException(
                    "Actor authority no longer references the deferred Join completion");
            }
            return published(current, completion);
        });
    }

    public CompletionStage<Published> recoverSuccessor(
        String staleReference,
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor) {
        return read(actor.actorId()).thenApply(current -> {
            if (current.publication().reference().equals(staleReference)) {
                throw new IllegalStateException(
                    "published deferred Join root is missing");
            }
            validateActor(current.root(), actor);
            var completion = find(
                current.root().terminalCompletions(), operationId);
            if (completion == null) {
                throw new IllegalStateException(
                    "successor root does not contain the deferred Join completion");
            }
            return published(current, completion);
        });
    }

    private CompletionStage<Current> read(String actorId) {
        String key = ZLinkAuthorityKeyCodec.actor(actorId);
        return authority.read(key, NEVER).thenCompose(result -> {
            if (!(result instanceof ZLinkAuthoritySnapshot snapshot)) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Actor authority is missing for deferred Join completion"));
            }
            var publication =
                ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                    snapshot.payload());
            if (publication == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Actor authority has no canonical relocation root"));
            }
            return load(
                    publication.reference(),
                    publication.checksumCrc32c())
                .thenApply(loaded ->
                    new Current(
                        key,
                        snapshot,
                        publication,
                        loaded.root(),
                        loaded.inventoryDigest()));
        });
    }

    private CompletionStage<Current> publish(
        Current current,
        ZLinkServiceRelocationEnvelopeCodec.Envelope successor) {
        byte[] root = ZLinkServiceRelocationEnvelopeCodec.encodeSuccessor(
            successor,
            successor.participantProgress(),
            successor.terminalCompletions());
        return ZLinkRelocationTreeStore.put(
                relocation,
                root,
                current.inventoryDigest(),
                RETENTION,
                NEVER)
            .thenCompose(stored -> authority.compareExchange(
                current.authorityKey(),
                new ZLinkAuthorityExpectFound(
                    current.snapshot().storeVersion()),
                new ZLinkAuthorityPut(
                    ZLinkCanonicalRelocationAuthorityStateCodec.replaceRoot(
                        current.snapshot().payload(),
                        stored.root(),
                        successor),
                    ZLinkAuthorityGenerationTransition.PRESERVE,
                    Optional.empty(),
                    Optional.empty()),
                NEVER)
                .thenCompose(result -> {
                    if (!(result instanceof ZLinkAuthorityStored)) {
                        return ZLinkRelocationTreeStore.delete(
                                relocation,
                                stored.root().reference(),
                                NEVER)
                            .thenCompose(ignored ->
                                CompletableFuture.failedFuture(
                                    new IllegalStateException(
                                        "deferred Join authority CAS conflicted")));
                    }
                    return ZLinkRelocationTreeStore.delete(
                            relocation,
                            current.publication().reference(),
                            NEVER)
                        .thenCompose(ignored ->
                            read(current.root().object().objectId()));
                }));
    }

    private CompletionStage<
        Loaded> load(
            String reference,
            long checksumCrc32c) {
        return relocation.get(reference, NEVER).thenCompose(result -> {
            if (result instanceof ZLinkRelocationMissing) {
                return CompletableFuture.failedFuture(
                    new CanonicalRootMissingException(
                        "deferred Join canonical relocation root is missing"));
            }
            if (!(result instanceof ZLinkRelocationFound found)
                || crc32c(found.payload()) != checksumCrc32c) {
                return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DATA_LOST,
                        "deferred Join canonical relocation root checksum is invalid"));
            }
            return ZLinkRelocationTreeStore.read(
                    relocation,
                    reference,
                    checksumCrc32c,
                    NEVER)
                .thenApply(read -> new Loaded(
                    ZLinkServiceRelocationEnvelopeCodec.decode(
                        read.logicalRoot()),
                    read.inventoryDigest()));
        });
    }

    private static Published published(
        Current current,
        ZLinkServiceRelocationEnvelopeCodec.Completion completion) {
        byte[] rawReply = completion.payload() == null
            ? new byte[0]
            : RELOCATION_CONTENT_TYPE.equals(
                completion.payload().contentType())
                ? decodeRelocationCompletion(completion).rawReply()
                : completion.payload().bytes();
        return new Published(
            current.publication().reference(),
            current.publication().checksumCrc32c(),
            completion.deliveryState(),
            new ZLinkActorJoinOperationId(
                completion.operationHigh(),
                completion.operationLow()),
            current.root().object().objectId(),
            current.root().object().objectGeneration(),
            rawReply);
    }

    private static void validateActor(
        ZLinkServiceRelocationEnvelopeCodec.Envelope root,
        ZLinkBackendActorRef actor) {
        if (root.object().kind() != 1
            || !root.object().objectId().equals(actor.actorId())
            || root.object().objectGeneration() != actor.generation()) {
            throw new IllegalStateException(
                "deferred Join Actor generation fence is stale");
        }
    }

    private static long onlyParticipant(
        ZLinkServiceRelocationEnvelopeCodec.Envelope root) {
        // Deferred Join moves one Actor independently. A User Spot aggregate
        // has object kind USER_SPOT and is rejected by validateActor before
        // this method; its sealed Actor lanes cannot start a concurrent Join.
        if (root.applicationStates().size() != 1
            || root.participantProgress().size() != 1) {
            throw new IllegalStateException(
                "deferred Join completion requires a standalone Actor root");
        }
        return root.participantProgress().getFirst().participantId();
    }

    private static ZLinkServiceRelocationEnvelopeCodec.Completion find(
        List<ZLinkServiceRelocationEnvelopeCodec.Completion> completions,
        ZLinkActorJoinOperationId operationId) {
        return completions.stream()
            .filter(value ->
                value.operationHigh() == operationId.high()
                    && value.operationLow() == operationId.low())
            .findFirst()
            .orElse(null);
    }

    private static void validateCompletion(
        ZLinkServiceRelocationEnvelopeCodec.Completion completion,
        ZLinkBackendActorRef actor,
        byte[] rawReply) {
        if (completion.payload() == null
            || !PACKET.equals(completion.payload().packetName())) {
            throw new IllegalStateException(
                "deferred Join completion conflicts with the published operation");
        }
        byte[] storedReply;
        if (CONTENT_TYPE.equals(completion.payload().contentType())) {
            storedReply = completion.payload().bytes();
        } else if (RELOCATION_CONTENT_TYPE.equals(
                completion.payload().contentType())) {
            storedReply = decodeRelocationCompletion(completion).rawReply();
        } else {
            throw new IllegalStateException(
                "deferred Join completion has an unsupported payload type");
        }
        if (!Arrays.equals(
                storedReply,
                rawReply == null ? new byte[0] : rawReply)) {
            throw new IllegalStateException(
                "deferred Join completion conflicts with the published operation");
        }
    }

    private static byte[] encodeRelocationCompletion(
        byte[] rawReply,
        byte[] sessionRouteCommand44) {
        byte[] reply = rawReply == null ? new byte[0] : rawReply.clone();
        byte[] route = sessionRouteCommand44 == null
            ? new byte[0]
            : sessionRouteCommand44.clone();
        try {
            var bytes = new java.io.ByteArrayOutputStream();
            try (var output = new java.io.DataOutputStream(bytes)) {
                output.writeInt(RELOCATION_COMPLETION_MAGIC);
                output.writeInt(RELOCATION_COMPLETION_VERSION);
                output.writeInt(reply.length);
                output.write(reply);
                output.writeInt(route.length);
                output.write(route);
            }
            return bytes.toByteArray();
        } catch (java.io.IOException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static RelocationCompletion decodeRelocationCompletion(
        ZLinkServiceRelocationEnvelopeCodec.Completion completion) {
        if (completion.payload() == null
            || !PACKET.equals(completion.payload().packetName())
            || !RELOCATION_CONTENT_TYPE.equals(
                completion.payload().contentType())) {
            throw new IllegalStateException(
                "direct Join relocation completion payload is invalid");
        }
        byte[] payload = completion.payload().bytes();
        try (var input = new java.io.DataInputStream(
                 new java.io.ByteArrayInputStream(payload))) {
            if (input.readInt() != RELOCATION_COMPLETION_MAGIC
                || input.readInt() != RELOCATION_COMPLETION_VERSION) {
                throw new IllegalStateException(
                    "direct Join relocation completion format is unsupported");
            }
            int replyLength = input.readInt();
            if (replyLength < 0 || replyLength > payload.length) {
                throw new IllegalStateException(
                    "direct Join relocation reply length is invalid");
            }
            byte[] reply = input.readNBytes(replyLength);
            int routeLength = input.readInt();
            if (routeLength < 0 || routeLength > payload.length) {
                throw new IllegalStateException(
                    "direct Join Session route length is invalid");
            }
            byte[] route = input.readNBytes(routeLength);
            if (reply.length != replyLength
                || route.length != routeLength
                || input.available() != 0) {
                throw new IllegalStateException(
                    "direct Join relocation completion is truncated");
            }
            return new RelocationCompletion(reply, route);
        } catch (java.io.IOException error) {
            throw new IllegalStateException(
                "direct Join relocation completion is invalid",
                error);
        }
    }

    private static long crc32c(byte[] payload) {
        java.util.zip.CRC32C checksum = new java.util.zip.CRC32C();
        checksum.update(payload, 0, payload.length);
        return checksum.getValue();
    }

    public record Published(
        String reference,
        long checksumCrc32c,
        int cursor,
        ZLinkActorJoinOperationId operationId,
        String actorId,
        long objectGeneration,
        byte[] rawReply) {
        public Published {
            rawReply = Objects.requireNonNull(rawReply, "rawReply").clone();
        }
        @Override public byte[] rawReply() { return rawReply.clone(); }
    }

    public record PreparedPublication(
        Published published,
        ZLinkAggregateFence fence) {
        public PreparedPublication {
            Objects.requireNonNull(published, "published");
            Objects.requireNonNull(fence, "fence");
        }
    }

    public record PreparedRoot(
        byte[] applicationState,
        List<ZLinkAsyncSerialQueue.QueuedRecord> acceptedJournal,
        byte[] sessionRouteCommand44) {
        public PreparedRoot {
            applicationState =
                Objects.requireNonNull(applicationState, "applicationState")
                    .clone();
            acceptedJournal =
                List.copyOf(Objects.requireNonNull(
                    acceptedJournal,
                    "acceptedJournal"));
            sessionRouteCommand44 =
                Objects.requireNonNull(
                    sessionRouteCommand44,
                    "sessionRouteCommand44").clone();
        }

        @Override public byte[] applicationState() {
            return applicationState.clone();
        }

        @Override public byte[] sessionRouteCommand44() {
            return sessionRouteCommand44.clone();
        }
    }

    private record RelocationCompletion(
        byte[] rawReply,
        byte[] sessionRouteCommand44) {
        private RelocationCompletion {
            rawReply = rawReply.clone();
            sessionRouteCommand44 = sessionRouteCommand44.clone();
        }

        @Override public byte[] rawReply() {
            return rawReply.clone();
        }

        @Override public byte[] sessionRouteCommand44() {
            return sessionRouteCommand44.clone();
        }
    }

    private record Current(
        String authorityKey,
        ZLinkAuthoritySnapshot snapshot,
        ZLinkCanonicalRelocationAuthorityStateCodec.Published publication,
        ZLinkServiceRelocationEnvelopeCodec.Envelope root,
        byte[] inventoryDigest) {
        Current {
            inventoryDigest = inventoryDigest.clone();
        }
        @Override public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }
    }

    private record Loaded(
        ZLinkServiceRelocationEnvelopeCodec.Envelope root,
        byte[] inventoryDigest) {
        Loaded {
            inventoryDigest = inventoryDigest.clone();
        }
        @Override public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }
    }

    public static final class CanonicalRootUnavailableException
        extends ZLinkFrameworkException {
        CanonicalRootUnavailableException(String message) {
            super(ZLinkFrameworkErrorKind.DATA_LOST, message);
        }
    }

    public static final class CanonicalRootMissingException
        extends ZLinkFrameworkException {
        CanonicalRootMissingException(String message) {
            super(ZLinkFrameworkErrorKind.DATA_LOST, message);
        }
    }
}
