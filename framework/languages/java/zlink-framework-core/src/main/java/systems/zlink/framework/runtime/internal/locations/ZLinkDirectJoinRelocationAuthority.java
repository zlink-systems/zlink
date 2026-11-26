package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

import java.util.List;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.zip.CRC32C;

/** Owns the canonical target-only Location Store transition for a direct Actor Join relocation. */
public final class ZLinkDirectJoinRelocationAuthority {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkDirectJoinRelocationAuthority.class.getName());
    private static final ZLinkStoreCancellation NEVER = () -> false;

    private final ZLinkLocationRepository authority;

    /** Immutable Actor tenure accepted from the committed Location row. */
    public record CommittedActorTenure(
            ZLinkBackendActorRef actor,
            long targetNodeGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration) {
        public CommittedActorTenure {
            Objects.requireNonNull(actor, "actor");
            if (actor.generation() <= 0
                    || targetNodeGeneration == 0
                    || authorityOwnerGeneration <= 0
                    || ownerLeaseGeneration <= 0) {
                throw new IllegalArgumentException(
                        "committed Actor tenure generations must be positive");
            }
        }
    }

    public ZLinkDirectJoinRelocationAuthority(ZLinkLocationRepository authority) {
        this.authority = Objects.requireNonNull(authority, "authority");
    }

    /**
     * Prepares a direct Actor Join as one canonical relocation aggregate. The returned fence is
     * committed by the target before it exposes the restored Actor or invokes the application
     * callback.
     */
    public CompletionStage<CommittedActorTenure> commitPrepared(
            UUID aggregateId, long aggregateGeneration, ZLinkBackendActorRef actor) {
        Objects.requireNonNull(actor, "actor");
        return authority
                .commitAggregate(
                        new ZLinkAggregateFence(aggregateId, aggregateGeneration),
                        new ZLinkAggregateCommitIntent(
                                ZLinkAuthorityKeyCodec.actor(actor.actorId()), 1, null),
                        NEVER)
                .thenCompose(
                        result ->
                                result == ZLinkAggregateCommitResult.COMMITTED
                                                || result
                                                        == ZLinkAggregateCommitResult
                                                                .ALREADY_COMMITTED
                                        ? readCommittedTenure(
                                                aggregateId, aggregateGeneration, actor)
                                        : CompletableFuture.failedFuture(
                                                new IllegalStateException(
                                                        "direct Join relocation aggregate commit"
                                                                + " failed: "
                                                                + result)));
    }

    private CompletionStage<CommittedActorTenure> readCommittedTenure(
            UUID aggregateId, long aggregateGeneration, ZLinkBackendActorRef actor) {
        return authority
                .read(ZLinkAuthorityKeyCodec.actor(actor.actorId()), NEVER)
                .thenCompose(
                        result -> {
                            if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                                    || snapshot.objectGeneration() != actor.generation()) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "direct Join Actor authority is missing after"
                                                        + " commit"));
                            }
                            var publication =
                                    ZLinkAuthorityRelocation.publication(
                                            snapshot, aggregateGeneration);
                            if (publication == null
                                    || !publication.aggregateId().equals(aggregateId)
                                    || publication.aggregateGeneration() != aggregateGeneration
                                    || !publication.targetOwnerId().equals(snapshot.ownerId())
                                    || publication.targetOwnerLeaseGeneration()
                                            != snapshot.ownerLeaseGeneration()
                                    || !publication
                                            .targetNodeRid()
                                            .equals(snapshot.allocation().descriptor().rid())
                                    || publication.targetNodeGeneration()
                                            != snapshot.allocation()
                                                    .descriptorLifecycleGeneration()) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "direct Join canonical owner fence differs after"
                                                        + " commit"));
                            }
                            var payload =
                                    new ZLinkActorAuthorityPayloadCodec()
                                            .decode(publication.applicationPayload())
                                            .orElse(null);
                            if (payload == null
                                    || !payload.actorId().equals(actor.actorId())
                                    || !payload.ownerId().equals(snapshot.ownerId())
                                    || payload.ownerLeaseGeneration()
                                            != snapshot.ownerLeaseGeneration()
                                    || !payload.meshName()
                                            .equals(snapshot.allocation().descriptor().meshName())
                                    || !payload.nodeRid()
                                            .equals(snapshot.allocation().descriptor().rid())
                                    || payload.nodeGeneration()
                                            != snapshot.allocation().descriptorLifecycleGeneration()
                                    || snapshot.allocation().objectKind()
                                            != ZLinkPlacementObjectKind.ACTOR) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "direct Join Actor authority owner differs after"
                                                        + " commit"));
                            }
                            //  The direct-transfer manifest is the handoff root; its
                            //  relocation identity is validated in loadPrepared, so a
                            //  committed tenure needs no store read-back here.
                            return CompletableFuture.completedFuture(
                                    new CommittedActorTenure(
                                            new ZLinkBackendActorRef(
                                                    snapshot.allocation().descriptor().rid(),
                                                    actor.actorId(),
                                                    snapshot.objectGeneration()),
                                            snapshot.allocation().descriptorLifecycleGeneration(),
                                            snapshot.authorityOwnerGeneration(),
                                            snapshot.ownerLeaseGeneration()));
                        });
    }

    /** Publishes Ready after target callbacks and temporary replay have ended. */
    public CompletionStage<Void> publishTargetReady(
            ZLinkBackendActorRef actor, UUID aggregateId, long aggregateGeneration) {
        Objects.requireNonNull(actor, "actor");
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actor.actorId());
        return authority
                .read(authorityKey, NEVER)
                .thenCompose(
                        result -> {
                            if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                                    || snapshot.objectGeneration() != actor.generation()) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "deferred Join Actor authority changed before"
                                                        + " Ready"));
                            }
                            var codec = new ZLinkActorAuthorityPayloadCodec();
                            var authorityPayload =
                                    codec.decode(snapshot.payload())
                                            .orElseThrow(
                                                    () ->
                                                            new IllegalStateException(
                                                                    "deferred Join Actor authority"
                                                                            + " payload is invalid"));
                            if (authorityPayload.state()
                                            == ZLinkActorAuthorityPayloadCodec.State.READY
                                    && authorityPayload.actorId().equals(actor.actorId())
                                    && authorityPayload.nodeRid().equals(actor.nodeRid())) {
                                return CompletableFuture.completedFuture(null);
                            }
                            var publication =
                                    ZLinkAuthorityRelocation.publication(
                                            snapshot, aggregateGeneration);
                            if (authorityPayload.state()
                                            != ZLinkActorAuthorityPayloadCodec.State.CREATING
                                    || publication == null
                                    || !publication.aggregateId().equals(aggregateId)
                                    || publication.aggregateGeneration() != aggregateGeneration
                                    || !authorityPayload.actorId().equals(actor.actorId())
                                    || !authorityPayload.nodeRid().equals(actor.nodeRid())
                                    || !authorityPayload
                                            .ownerId()
                                            .equals(publication.targetOwnerId())
                                    || authorityPayload.ownerLeaseGeneration()
                                            != publication.targetOwnerLeaseGeneration()) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "deferred Join target authority changed before"
                                                        + " Ready"));
                            }
                            byte[] ready =
                                    codec.encode(
                                            ZLinkActorAuthorityPayloadCodec.State.READY,
                                            authorityPayload.stableType(),
                                            authorityPayload.actorId(),
                                            authorityPayload.currentSpotId(),
                                            authorityPayload.currentSpotGeneration(),
                                            authorityPayload.currentSpotKind(),
                                            authorityPayload.ownerId(),
                                            authorityPayload.ownerLeaseGeneration(),
                                            authorityPayload.meshName(),
                                            authorityPayload.nodeRid(),
                                            authorityPayload.nodeGeneration());
                            return authority
                                    .compareExchange(
                                            authorityKey,
                                            new ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                                            ZLinkAuthorityPut.relocation(ready, null),
                                            NEVER)
                                    .thenCompose(
                                            stored -> {
                                                if (stored instanceof ZLinkAuthorityStored) {
                                                    cleanupReadyRelocation(publication);
                                                    return CompletableFuture.completedFuture(null);
                                                }
                                                return authority
                                                        .read(authorityKey, NEVER)
                                                        .thenCompose(
                                                                reloaded ->
                                                                        reloaded
                                                                                                instanceof
                                                                                                ZLinkAuthoritySnapshot
                                                                                                        current
                                                                                        && current
                                                                                                        .objectGeneration()
                                                                                                == actor
                                                                                                        .generation()
                                                                                        && codec.decode(
                                                                                                        current
                                                                                                                .payload())
                                                                                                .filter(
                                                                                                        value ->
                                                                                                                value
                                                                                                                                .state()
                                                                                                                        == ZLinkActorAuthorityPayloadCodec
                                                                                                                                .State
                                                                                                                                .READY)
                                                                                                .filter(
                                                                                                        value ->
                                                                                                                value.actorId()
                                                                                                                        .equals(
                                                                                                                                actor
                                                                                                                                        .actorId()))
                                                                                                .filter(
                                                                                                        value ->
                                                                                                                value.nodeRid()
                                                                                                                        .equals(
                                                                                                                                actor
                                                                                                                                        .nodeRid()))
                                                                                                .isPresent()
                                                                                ? CompletableFuture
                                                                                        .completedFuture(
                                                                                                null)
                                                                                : CompletableFuture
                                                                                        .failedFuture(
                                                                                                new IllegalStateException(
                                                                                                        "deferred"
                                                                                                                + " Join"
                                                                                                                + " Ready"
                                                                                                                + " CAS conflicted")));
                                            });
                        });
    }

    private void cleanupReadyRelocation(
            ZLinkCanonicalRelocationAuthorityStateCodec.Published publication) {
        ZLinkAggregateFence fence =
                new ZLinkAggregateFence(
                        publication.aggregateId(), publication.aggregateGeneration());
        try {
            authority
                    .readAggregateProgress(fence, NEVER)
                    .thenCompose(
                            marker ->
                                    marker.isPresent()
                                            ? authority.removeAggregateProgress(
                                                    fence, marker.get().storeVersion(), NEVER)
                                            : CompletableFuture.completedFuture(true))
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure != null) {
                                    LOGGER.log(
                                            Level.WARNING,
                                            "Ready relocation progress cleanup failed",
                                            failure);
                                }
                            });
        } catch (RuntimeException failure) {
            LOGGER.log(Level.WARNING, "Ready relocation progress cleanup failed", failure);
        }
    }

    public CompletionStage<Void> abortPrepared(UUID aggregateId, long aggregateGeneration) {
        return authority
                .abortAggregate(new ZLinkAggregateFence(aggregateId, aggregateGeneration), NEVER)
                .thenCompose(
                        result -> {
                            if (result != ZLinkAggregateAbortResult.ABORTED
                                    && result != ZLinkAggregateAbortResult.ALREADY_ABORTED) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "direct Join relocation aggregate abort failed: "
                                                        + result));
                            }
                            return CompletableFuture.<Void>completedFuture(null);
                        });
    }

    public CompletionStage<PreparedRoot> loadPrepared(
            byte[] rootBytes,
            long checksumCrc32c,
            ZLinkBackendActorRef actor,
            UUID relocationId,
            boolean restoreSnapshot) {
        return verify(rootBytes, checksumCrc32c)
                .thenApply(
                        root -> {
                            validateActor(root, actor);
                            if (root.relocationHigh() != relocationId.getMostSignificantBits()
                                    || root.relocationLow()
                                            != relocationId.getLeastSignificantBits()
                                    || root.applicationStates().size() != 1
                                    || !root.timerRegistrations().isEmpty()
                                    || !root.pendingTimerTicks().isEmpty()) {
                                throw new IllegalStateException(
                                        "direct Join canonical relocation root is invalid");
                            }
                            var state = root.applicationStates().getFirst();
                            if (state.participantId() != 1 || state.hasState() != restoreSnapshot) {
                                throw new IllegalStateException(
                                        "direct Join relocation state policy differs");
                            }
                            List<ZLinkSerialExecutionQueue.QueuedRecord> journal =
                                    root.savedWork().stream()
                                            .map(
                                                    value -> {
                                                        if (value.participantId() != 1) {
                                                            throw new IllegalStateException(
                                                                    "direct Join journal references"
                                                                            + " another participant");
                                                        }
                                                        return new ZLinkSerialExecutionQueue
                                                                .QueuedRecord(
                                                                value.sequence(), value.rawEntry());
                                                    })
                                            .toList();
                            return new PreparedRoot(state.payload(), journal);
                        });
    }

    private static CompletionStage<ZLinkServiceRelocationEnvelopeCodec.Envelope> verify(
            byte[] rootBytes, long checksumCrc32c) {
        Objects.requireNonNull(rootBytes, "rootBytes");
        if (rootBytes.length == 0) {
            return CompletableFuture.failedFuture(
                    new CanonicalRootMissingException(
                            "deferred Join canonical relocation root is missing"));
        }
        if (crc32c(rootBytes) != checksumCrc32c) {
            //  A checksum mismatch over TCP is a defect signal, never retried
            //  and never restored from a partial payload (spec 28 §4.3).
            return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DATA_LOST,
                            "deferred Join canonical relocation root checksum is invalid"));
        }
        try {
            return CompletableFuture.completedFuture(
                    ZLinkServiceRelocationEnvelopeCodec.decode(rootBytes));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private static void validateActor(
            ZLinkServiceRelocationEnvelopeCodec.Envelope root, ZLinkBackendActorRef actor) {
        if (root.object().kind() != 1
                || !root.object().objectId().equals(actor.actorId())
                || root.object().objectGeneration() != actor.generation()) {
            throw new IllegalStateException("deferred Join Actor generation fence is stale");
        }
    }

    private static long crc32c(byte[] payload) {
        CRC32C checksum = new CRC32C();
        checksum.update(payload, 0, payload.length);
        return checksum.getValue();
    }

    public record PreparedRoot(
            byte[] applicationState, List<ZLinkSerialExecutionQueue.QueuedRecord> acceptedJournal) {
        public PreparedRoot {
            applicationState = Objects.requireNonNull(applicationState, "applicationState").clone();
            acceptedJournal =
                    List.copyOf(Objects.requireNonNull(acceptedJournal, "acceptedJournal"));
        }

        @Override
        public byte[] applicationState() {
            return applicationState.clone();
        }
    }

    public static final class CanonicalRootMissingException extends ZLinkFrameworkException {
        CanonicalRootMissingException(String message) {
            super(ZLinkFrameworkErrorKind.DATA_LOST, message);
        }
    }
}
