package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.stream.Collectors;
import java.util.zip.CRC32C;

/**
 * Publishes all participant routes of one direct-transfer relocation through one bounded Location
 * Store aggregate commit. The handoff payload lives only in source memory and on the wire (spec 28
 * §4.2) — no store round-trip.
 */
public final class ZLinkAggregateRelocationCoordinator {
    private static final ZLinkStoreCancellation NEVER_CANCELLED = () -> false;

    final ZLinkLocationRepository authorityStore;
    private final Predicate<ZLinkLocationOwnerToken> ownerLeaseValid;

    public ZLinkAggregateRelocationCoordinator(ZLinkLocationRepository authorityStore) {
        this(authorityStore, ignored -> false);
    }

    public ZLinkAggregateRelocationCoordinator(
            ZLinkLocationRepository authorityStore,
            Predicate<ZLinkLocationOwnerToken> ownerLeaseValid) {
        this.authorityStore = Objects.requireNonNull(authorityStore, "authorityStore");
        this.ownerLeaseValid = Objects.requireNonNull(ownerLeaseValid, "ownerLeaseValid");
    }

    public CompletionStage<Prepared> prepare(Request request, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        return prepareAuthority(request, cancellation);
    }

    public CompletionStage<Published> commit(
            Prepared prepared, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(prepared, "prepared");
        Objects.requireNonNull(cancellation, "cancellation");
        return commitAuthority(prepared, cancellation);
    }

    public record ExpectedParticipant(
            String authorityKey, long objectGeneration, long sourceAuthorityOwnerGeneration) {
        public ExpectedParticipant {
            if (authorityKey == null
                    || authorityKey.isBlank()
                    || objectGeneration <= 0
                    || sourceAuthorityOwnerGeneration <= 0) {
                throw new IllegalArgumentException("published participant fence is invalid");
            }
        }
    }

    /**
     * Replaces target-published relocation slots with steady authority payloads. Ownership and both
     * object generations are preserved by StoreVersion CAS.
     */
    public CompletionStage<Void> normalizePublishedAggregate(
            List<ExpectedParticipant> participants,
            ZLinkAggregateFence activatedFence,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkStoreCancellation cancellation) {
        List<ExpectedParticipant> expected =
                List.copyOf(Objects.requireNonNull(participants, "participants"));
        Objects.requireNonNull(activatedFence, "activatedFence");
        Objects.requireNonNull(targetOwner, "targetOwner");
        Objects.requireNonNull(cancellation, "cancellation");
        if (expected.isEmpty()) {
            throw new IllegalArgumentException("completed aggregate participant fence is invalid");
        }
        return advancePublished(expected, activatedFence, "activated", cancellation)
                .thenCompose(
                        ignored ->
                                advancePublished(
                                        expected, activatedFence, "cleaning", cancellation))
                .thenCompose(
                        ignored ->
                                advancePublished(
                                        expected, activatedFence, "completed", cancellation))
                .thenCompose(
                        ignoredPhase -> {
                            List<Participant> steady = new ArrayList<>();
                            var alreadySteady = new AtomicInteger();
                            CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
                            for (ExpectedParticipant participant : expected) {
                                reads =
                                        reads.thenCompose(
                                                ignored ->
                                                        authorityStore
                                                                .read(
                                                                        participant.authorityKey(),
                                                                        cancellation)
                                                                .thenCompose(
                                                                        read ->
                                                                                collectSteadyParticipant(
                                                                                        participant,
                                                                                        activatedFence,
                                                                                        targetOwner,
                                                                                        read,
                                                                                        steady,
                                                                                        alreadySteady)));
                            }
                            CompletionStage<Void> participantReads = reads;
                            return authorityStore
                                    .readAggregateProgress(activatedFence, cancellation)
                                    .thenCompose(
                                            marker -> {
                                                return participantReads.thenCompose(
                                                        ignored -> {
                                                            if (alreadySteady.get() + steady.size()
                                                                    != expected.size()) {
                                                                return failed(
                                                                        new RelocationDataLostException(
                                                                                "published"
                                                                                        + " relocation"
                                                                                        + " normalization"
                                                                                        + " has an"
                                                                                        + " unknown"
                                                                                        + " participant"
                                                                                        + " state"));
                                                            }
                                                            CompletionStage<Void> writes =
                                                                    CompletableFuture
                                                                            .completedFuture(null);
                                                            for (Participant participant : steady) {
                                                                writes =
                                                                        writes.thenCompose(
                                                                                unused ->
                                                                                        normalizeParticipant(
                                                                                                participant,
                                                                                                targetOwner,
                                                                                                cancellation));
                                                            }
                                                            return writes.thenCompose(
                                                                    ignoredWrite ->
                                                                            removeProgressAfterNormalization(
                                                                                    activatedFence,
                                                                                    marker,
                                                                                    cancellation));
                                                        });
                                            });
                        });
    }

    private CompletionStage<Void> normalizeParticipant(
            Participant participant,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkStoreCancellation cancellation) {
        return authorityStore
                .compareExchange(
                        participant.authorityKey(),
                        new ZLinkAuthorityExpectFound(participant.expectedStoreVersion()),
                        ZLinkAuthorityPut.relocation(
                                participant.applicationAuthorityPayload(), null),
                        cancellation)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkAuthorityStored) {
                                return CompletableFuture.completedFuture(null);
                            }
                            return authorityStore
                                    .read(participant.authorityKey(), cancellation)
                                    .thenCompose(
                                            read ->
                                                    read instanceof ZLinkAuthoritySnapshot snapshot
                                                                    && isSteadySnapshot(
                                                                            snapshot,
                                                                            participant,
                                                                            targetOwner)
                                                            ? CompletableFuture.completedFuture(
                                                                    null)
                                                            : failed(
                                                                    new RelocationDataLostException(
                                                                            "steady authority"
                                                                                    + " normalization"
                                                                                    + " CAS conflicted:"
                                                                                    + " "
                                                                                    + participant
                                                                                            .authorityKey())));
                        });
    }

    private static boolean isSteadySnapshot(
            ZLinkAuthoritySnapshot snapshot,
            Participant participant,
            ZLinkLocationOwnerToken targetOwner) {
        return snapshot.objectGeneration() == participant.objectGeneration()
                && snapshot.authorityOwnerGeneration() == participant.authorityOwnerGeneration()
                && snapshot.ownerId().equals(targetOwner.ownerId())
                && snapshot.ownerLeaseGeneration() == targetOwner.leaseGeneration()
                && snapshot.relocation() == null
                && ZLinkCanonicalRelocationAuthorityStateCodec.decode(snapshot.payload()) == null;
    }

    private CompletionStage<Void> removeProgressAfterNormalization(
            ZLinkAggregateFence fence,
            Optional<ZLinkAggregateProgressSnapshot> marker,
            ZLinkStoreCancellation cancellation) {
        if (marker.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        return authorityStore
                .removeAggregateProgress(fence, marker.get().storeVersion(), cancellation)
                .thenCompose(
                        removed -> {
                            if (removed) {
                                return CompletableFuture.completedFuture(null);
                            }
                            return authorityStore
                                    .readAggregateProgress(fence, cancellation)
                                    .thenCompose(
                                            current -> {
                                                if (current.isEmpty()) {
                                                    return CompletableFuture.completedFuture(null);
                                                }
                                                return authorityStore
                                                        .removeAggregateProgress(
                                                                fence,
                                                                current.get().storeVersion(),
                                                                cancellation)
                                                        .thenCompose(
                                                                retried ->
                                                                        retried
                                                                                ? CompletableFuture
                                                                                        .completedFuture(
                                                                                                null)
                                                                                : failed(
                                                                                        new RelocationDataLostException(
                                                                                                "aggregate"
                                                                                                        + " progress"
                                                                                                        + " removal"
                                                                                                        + " lost"
                                                                                                        + " its StoreVersion"
                                                                                                        + " race")));
                                            });
                        });
    }

    private CompletionStage<Void> collectSteadyParticipant(
            ExpectedParticipant participant,
            ZLinkAggregateFence activatedFence,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkAuthorityReadResult read,
            List<Participant> steady,
            AtomicInteger alreadySteady) {
        if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || !matchesSteadyOwner(snapshot, participant, targetOwner)) {
            return failed(
                    new RelocationDataLostException(
                            "completed relocation participant has a different owner fence: "
                                    + participant.authorityKey()));
        }
        var publication =
                ZLinkAuthorityRelocation.publication(
                        snapshot, activatedFence.aggregateGeneration());
        if (publication == null) {
            alreadySteady.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
        if (!matchesCompletedParticipant(
                snapshot, publication, participant, activatedFence, targetOwner)) {
            return failed(
                    new RelocationDataLostException(
                            "completed relocation participant has a different normalization fence: "
                                    + participant.authorityKey()));
        }
        steady.add(
                new Participant(
                        participant.authorityKey(),
                        snapshot.allocation().objectKind(),
                        snapshot.objectGeneration(),
                        snapshot.authorityOwnerGeneration(),
                        snapshot.storeVersion(),
                        ZLinkAuthorityGenerationTransition.PRESERVE,
                        publication.applicationPayload(),
                        new byte[0]));
        return CompletableFuture.completedFuture(null);
    }

    private static boolean matchesCompletedParticipant(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkCanonicalRelocationAuthorityStateCodec.Published publication,
            ExpectedParticipant participant,
            ZLinkAggregateFence activatedFence,
            ZLinkLocationOwnerToken targetOwner) {
        return publication != null
                && snapshot.objectGeneration() == participant.objectGeneration()
                && ownerGenerationAdvanced(
                        participant.sourceAuthorityOwnerGeneration(),
                        snapshot.authorityOwnerGeneration())
                && snapshot.ownerId().equals(targetOwner.ownerId())
                && snapshot.ownerLeaseGeneration() == targetOwner.leaseGeneration()
                && publication.aggregateId().equals(activatedFence.aggregateId())
                && publication.aggregateGeneration() == activatedFence.aggregateGeneration()
                && publication.targetOwnerId().equals(targetOwner.ownerId())
                && publication.targetOwnerLeaseGeneration() == targetOwner.leaseGeneration()
                && publication.targetNodeRid().equals(snapshot.allocation().descriptor().rid())
                && publication.targetNodeGeneration()
                        == snapshot.allocation().descriptorLifecycleGeneration();
    }

    private static boolean matchesSteadyOwner(
            ZLinkAuthoritySnapshot snapshot,
            ExpectedParticipant participant,
            ZLinkLocationOwnerToken targetOwner) {
        return snapshot.objectGeneration() == participant.objectGeneration()
                && ownerGenerationAdvanced(
                        participant.sourceAuthorityOwnerGeneration(),
                        snapshot.authorityOwnerGeneration())
                && snapshot.ownerId().equals(targetOwner.ownerId())
                && snapshot.ownerLeaseGeneration() == targetOwner.leaseGeneration();
    }

    public CompletionStage<Void> abort(Prepared prepared) {
        Objects.requireNonNull(prepared, "prepared");
        return authorityStore
                .abortAggregate(prepared.fence(), NEVER_CANCELLED)
                .thenCompose(
                        result -> {
                            if (result != ZLinkAggregateAbortResult.ABORTED
                                    && result != ZLinkAggregateAbortResult.ALREADY_ABORTED) {
                                return failed(
                                        new IllegalStateException(
                                                "aggregate relocation abort was rejected: "
                                                        + result));
                            }
                            return CompletableFuture.<Void>completedFuture(null);
                        });
    }

    /** Releases target preparation and its inventory before cutover. */
    public CompletionStage<Void> abortPreparedFence(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(cancellation, "cancellation");
        return authorityStore
                .abortAggregate(fence, cancellation)
                .thenCompose(
                        result ->
                                result == ZLinkAggregateAbortResult.ABORTED
                                                || result
                                                        == ZLinkAggregateAbortResult.ALREADY_ABORTED
                                        ? CompletableFuture.completedFuture(null)
                                        : failed(new AuthorityConflictException(result)));
    }

    /** Seals inventory before publishing the source preparing phase. */
    public CompletionStage<ZLinkAuthoritySnapshot> prepareSource(
            List<ExpectedParticipant> participants,
            String primaryKey,
            ZLinkAggregateFence fence,
            long applicationVersion,
            Function<ZLinkAuthoritySnapshot, byte[]> authorityPayload,
            ZLinkStoreCancellation cancellation) {
        return sealSource(participants, primaryKey, fence, authorityPayload, cancellation)
                .thenCompose(inventory -> inventory.beginCapture(applicationVersion, cancellation));
    }

    public CompletionStage<SealedSource> sealSource(
            List<ExpectedParticipant> participants,
            String primaryKey,
            ZLinkAggregateFence fence,
            Function<ZLinkAuthoritySnapshot, byte[]> authorityPayload,
            ZLinkStoreCancellation cancellation) {
        var primary = primaryParticipant(participants, primaryKey);
        return readSourceParticipant(primary, cancellation)
                .thenCompose(
                        snapshot -> {
                            var previous = snapshot.relocation();
                            if (previous != null) {
                                return CompletableFuture.completedFuture(
                                        new SealedSource(primaryKey, snapshot, fence, null));
                            }
                            var entries =
                                    new ArrayList<ZLinkAggregateParticipant>(participants.size());
                            CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
                            for (var participant :
                                    participants.stream()
                                            .sorted(
                                                    Comparator.comparing(
                                                            ExpectedParticipant::authorityKey,
                                                            ZLinkAggregateInventoryStore
                                                                    ::compareKeys))
                                            .toList()) {
                                reads =
                                        reads.thenCompose(
                                                ignored -> {
                                                    var read =
                                                            participant.equals(primary)
                                                                    ? CompletableFuture
                                                                            .completedFuture(
                                                                                    snapshot)
                                                                    : readSourceParticipant(
                                                                            participant,
                                                                            cancellation);
                                                    return read.thenAccept(
                                                            member -> {
                                                                if (!member.ownerId()
                                                                                .equals(
                                                                                        snapshot
                                                                                                .ownerId())
                                                                        || member
                                                                                        .ownerLeaseGeneration()
                                                                                != snapshot
                                                                                        .ownerLeaseGeneration()) {
                                                                    throw new IllegalStateException(
                                                                            "Source capture owner"
                                                                                    + " differs");
                                                                }
                                                                entries.add(
                                                                        new ZLinkAggregateParticipant(
                                                                                participant
                                                                                        .authorityKey(),
                                                                                member
                                                                                        .objectGeneration(),
                                                                                member
                                                                                        .authorityOwnerGeneration(),
                                                                                member
                                                                                        .storeVersion(),
                                                                                ZLinkAuthorityGenerationTransition
                                                                                        .NEW_OWNER,
                                                                                authorityPayload
                                                                                        .apply(
                                                                                                member),
                                                                                new byte[] {
                                                                                    '[', ']'
                                                                                }));
                                                            });
                                                });
                            }
                            return reads.thenCompose(
                                            ignored ->
                                                    authorityStore.sealAggregateInventory(
                                                            fence, entries, cancellation))
                                    .thenApply(
                                            digest ->
                                                    new SealedSource(
                                                            primaryKey, snapshot, fence, digest));
                        });
    }

    public final class SealedSource {
        private final String primaryKey;
        private final ZLinkAuthoritySnapshot snapshot;
        private final ZLinkAggregateFence fence;
        private final byte[] digest;

        private SealedSource(
                String primaryKey,
                ZLinkAuthoritySnapshot snapshot,
                ZLinkAggregateFence fence,
                byte[] digest) {
            this.primaryKey = primaryKey;
            this.snapshot = snapshot;
            this.fence = fence;
            this.digest = digest;
        }

        public CompletionStage<ZLinkAuthoritySnapshot> beginCapture(
                long applicationVersion, ZLinkStoreCancellation cancellation) {
            if (digest == null) return verifySourcePhase(snapshot, fence, applicationVersion);
            return writePhase(
                    primaryKey,
                    snapshot,
                    ZLinkAuthorityRelocation.preparing(
                            snapshot,
                            snapshot,
                            fence.aggregateId(),
                            HexFormat.of().formatHex(digest),
                            applicationVersion),
                    cancellation);
        }
    }

    public CompletionStage<ZLinkAuthoritySnapshot> captureSource(
            List<ExpectedParticipant> participants,
            String primaryKey,
            ZLinkAggregateFence fence,
            long applicationVersion,
            ZLinkStoreCancellation cancellation) {
        return readSourceParticipant(primaryParticipant(participants, primaryKey), cancellation)
                .thenCompose(snapshot -> verifySourcePhase(snapshot, fence, applicationVersion))
                .thenCompose(
                        snapshot -> {
                            var relocation = snapshot.relocation();
                            return relocation.hasReached("captured")
                                    ? CompletableFuture.completedFuture(snapshot)
                                    : writePhase(
                                            primaryKey,
                                            snapshot,
                                            relocation.phase("captured"),
                                            cancellation);
                        });
    }

    private static ExpectedParticipant primaryParticipant(
            List<ExpectedParticipant> participants, String primaryKey) {
        return participants.stream()
                .filter(participant -> participant.authorityKey().equals(primaryKey))
                .findFirst()
                .orElseThrow();
    }

    private CompletionStage<ZLinkAuthoritySnapshot> readSourceParticipant(
            ExpectedParticipant participant, ZLinkStoreCancellation cancellation) {
        return authorityStore
                .read(participant.authorityKey(), cancellation)
                .thenApply(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                                    || snapshot.objectGeneration() != participant.objectGeneration()
                                    || snapshot.authorityOwnerGeneration()
                                            != participant.sourceAuthorityOwnerGeneration()) {
                                throw new IllegalStateException(
                                        "Source capture authority fence differs");
                            }
                            return snapshot;
                        });
    }

    private static CompletionStage<ZLinkAuthoritySnapshot> verifySourcePhase(
            ZLinkAuthoritySnapshot snapshot, ZLinkAggregateFence fence, long applicationVersion) {
        var relocation = snapshot.relocation();
        if (relocation == null
                || relocation.aborted()
                || !relocation.relocationId().equals(fence.aggregateId())
                || relocation.applicationVersion() != applicationVersion) {
            return failed(
                    new RelocationDataLostException("source inventory has no matching phase"));
        }
        return CompletableFuture.completedFuture(snapshot);
    }

    public record SealedParticipant(
            ZLinkAuthorityEntry authority,
            String expectedStoreVersion,
            ZLinkAuthorityGenerationTransition ownerTransition,
            byte[] authorityPayload,
            byte[] membershipMutation) {}

    public CompletionStage<List<SealedParticipant>> readSealedInventory(
            ZLinkAggregateFence fence, String primaryKey, ZLinkStoreCancellation cancellation) {
        return readSealedInventory(fence, primaryKey, null, 0, null, 0, cancellation);
    }

    public CompletionStage<List<SealedParticipant>> readSealedInventory(
            ZLinkAggregateFence fence,
            String primaryKey,
            RoutingId targetRid,
            long targetNodeGeneration,
            ZLinkLocationOwnerToken targetOwner,
            long attempt,
            ZLinkStoreCancellation cancellation) {
        return authorityStore
                .read(primaryKey, cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot primary)
                                    || primary.relocation() == null
                                    || !primary.relocation()
                                            .relocationId()
                                            .equals(fence.aggregateId())) {
                                return failed(
                                        new RelocationDataLostException(
                                                "source inventory is not captured"));
                            }
                            var relocation = primary.relocation();
                            if (targetOwner == null) {
                                if (!relocation.phase().equals("captured")) {
                                    return failed(
                                            new RelocationDataLostException(
                                                    "source inventory is not captured"));
                                }
                            } else {
                                long current =
                                        relocation.restoreAttempt(
                                                targetRid, targetNodeGeneration, targetOwner);
                                if (current == 0
                                        || (attempt != current
                                                && !(relocation.phase().equals("prepared")
                                                        && current < Long.MAX_VALUE
                                                        && attempt == current + 1))) {
                                    return failed(
                                            ZLinkRequestFailureMapping.receivedFailure(
                                                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                                                    "Restore conflicts with representative"
                                                            + " authority",
                                                    (int)
                                                            ServiceWireConstants
                                                                    .FRAMEWORK_ERROR_ACTOR_LOCATION_STALE,
                                                    Map.of()));
                                }
                            }
                            String digest = primary.relocation().sourceInventorySha256();
                            return authorityStore
                                    .readAggregateInventory(
                                            fence, HexFormat.of().parseHex(digest), cancellation)
                                    .thenCompose(
                                            entries ->
                                                    bindSealedInventory(
                                                            entries,
                                                            primaryKey,
                                                            primary,
                                                            cancellation));
                        });
    }

    private CompletionStage<List<SealedParticipant>> bindSealedInventory(
            List<ZLinkAggregateInventoryStore.StoredParticipant> entries,
            String primaryKey,
            ZLinkAuthoritySnapshot primary,
            ZLinkStoreCancellation cancellation) {
        List<SealedParticipant> result = new ArrayList<>(entries.size());
        CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
        for (var entry : entries) {
            boolean root = entry.authorityKey().equals(primaryKey);
            reads =
                    reads.thenCompose(
                            ignored -> {
                                CompletionStage<ZLinkAuthorityReadResult> authority =
                                        root
                                                ? CompletableFuture.completedFuture(primary)
                                                : authorityStore.read(
                                                        entry.authorityKey(), cancellation);
                                return authority.thenAccept(
                                        read -> {
                                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                                                    || !(root
                                                                    ? primary.relocation()
                                                                            .coordinatorExpectedStoreVersion()
                                                                    : snapshot.storeVersion())
                                                            .equals(entry.expectedStoreVersion())
                                                    || !snapshot.ownerId().equals(primary.ownerId())
                                                    || snapshot.ownerLeaseGeneration()
                                                            != primary.ownerLeaseGeneration()) {
                                                throw new RelocationDataLostException(
                                                        "participant authority differs from sealed"
                                                                + " inventory: "
                                                                + entry.authorityKey());
                                            }
                                            result.add(
                                                    new SealedParticipant(
                                                            new ZLinkAuthorityEntry(
                                                                    entry.authorityKey(), snapshot),
                                                            entry.expectedStoreVersion(),
                                                            entry.ownerTransition(),
                                                            entry.authorityPayload(),
                                                            entry.membershipMutation()));
                                        });
                            });
        }
        return reads.thenApply(
                ignored -> {
                    if (result.stream()
                            .noneMatch(entry -> entry.authority().key().equals(primaryKey))) {
                        throw new RelocationDataLostException(
                                "sealed inventory omits the primary authority");
                    }
                    return List.copyOf(result);
                });
    }

    public CompletionStage<Void> abortSource(
            ExpectedParticipant primary,
            ZLinkAggregateFence fence,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkStoreCancellation cancellation) {
        return abortPreparedFence(fence, cancellation)
                .thenCompose(
                        ignored ->
                                abortSourceProgress(
                                        primary,
                                        fence.aggregateId(),
                                        ownerId,
                                        ownerLeaseGeneration,
                                        cancellation));
    }

    public CompletionStage<Void> abortSourceProgress(
            ExpectedParticipant primary,
            UUID relocationId,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkStoreCancellation cancellation) {
        return sourceTerminal(
                primary, relocationId, ownerId, ownerLeaseGeneration, "aborted", cancellation);
    }

    public CompletionStage<Void> clearSourceProgress(
            ExpectedParticipant primary,
            ZLinkAggregateFence fence,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkStoreCancellation cancellation) {
        return sourceTerminal(
                        primary,
                        fence.aggregateId(),
                        ownerId,
                        ownerLeaseGeneration,
                        null,
                        cancellation)
                .thenCompose(
                        ignored -> authorityStore.deleteAggregateInventory(fence, cancellation));
    }

    private CompletionStage<Void> sourceTerminal(
            ExpectedParticipant primary,
            UUID relocationId,
            String ownerId,
            long ownerLeaseGeneration,
            String phase,
            ZLinkStoreCancellation cancellation) {
        return authorityStore
                .read(primary.authorityKey(), cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                                    || snapshot.relocation() == null
                                    || !snapshot.relocation().relocationId().equals(relocationId)
                                    || snapshot.objectGeneration() != primary.objectGeneration()
                                    || snapshot.authorityOwnerGeneration()
                                            != primary.sourceAuthorityOwnerGeneration()
                                    || !snapshot.ownerId().equals(ownerId)
                                    || snapshot.ownerLeaseGeneration() != ownerLeaseGeneration) {
                                return CompletableFuture.completedFuture(null);
                            }
                            if (phase == null && !snapshot.relocation().phase().equals("aborted")) {
                                return failed(
                                        new IllegalStateException(
                                                "Source relocation cleanup requires Aborted"));
                            }
                            return writePhase(
                                            primary.authorityKey(),
                                            snapshot,
                                            phase == null
                                                    ? null
                                                    : snapshot.relocation().phase(phase),
                                            cancellation)
                                    .thenApply(done -> null);
                        });
    }

    private CompletionStage<ZLinkAuthoritySnapshot> writePhase(
            String key,
            ZLinkAuthoritySnapshot snapshot,
            ZLinkAuthorityRelocation relocation,
            ZLinkStoreCancellation cancellation) {
        return authorityStore
                .compareExchange(
                        key,
                        new ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                        ZLinkAuthorityPut.relocation(
                                ZLinkCanonicalRelocationAuthorityStateCodec
                                        .applicationPayloadOrOriginal(snapshot.payload()),
                                relocation),
                        cancellation)
                .thenCompose(
                        result -> {
                            if (!(result instanceof ZLinkAuthorityStored stored))
                                return failed(new AuthorityConflictException(result));
                            return CompletableFuture.completedFuture(
                                    new ZLinkAuthoritySnapshot(
                                            stored.storeVersion(),
                                            stored.payload(),
                                            stored.objectGeneration(),
                                            stored.authorityOwnerGeneration(),
                                            stored.ownerId(),
                                            stored.ownerLeaseGeneration(),
                                            stored.allocation(),
                                            snapshot.pendingCreation(),
                                            stored.storeNow(),
                                            relocation,
                                            snapshot.aggregateGeneration()));
                        });
    }

    public CompletionStage<Void> advancePublished(
            List<ExpectedParticipant> participants,
            ZLinkAggregateFence fence,
            String phase,
            ZLinkStoreCancellation cancellation) {
        CompletionStage<Void> writes = CompletableFuture.completedFuture(null);
        for (var participant : participants) {
            writes =
                    writes.thenCompose(
                            ignored ->
                                    authorityStore
                                            .read(participant.authorityKey(), cancellation)
                                            .thenCompose(
                                                    read -> {
                                                        if (!(read
                                                                        instanceof
                                                                        ZLinkAuthoritySnapshot
                                                                                snapshot)
                                                                || snapshot.objectGeneration()
                                                                        != participant
                                                                                .objectGeneration()
                                                                || !ownerGenerationAdvanced(
                                                                        participant
                                                                                .sourceAuthorityOwnerGeneration(),
                                                                        snapshot
                                                                                .authorityOwnerGeneration())
                                                                || (snapshot.relocation() != null
                                                                        && !snapshot.relocation()
                                                                                .relocationId()
                                                                                .equals(
                                                                                        fence
                                                                                                .aggregateId())))
                                                            return failed(
                                                                    new IllegalStateException(
                                                                            "Relocation phase"
                                                                                    + " authority"
                                                                                    + " differs"));
                                                        if (snapshot.relocation() != null
                                                                && snapshot.relocation().aborted()
                                                                && !phase.equals("aborted"))
                                                            return failed(
                                                                    new IllegalStateException(
                                                                            "Relocation was"
                                                                                    + " aborted"));
                                                        if (snapshot.relocation() == null
                                                                || snapshot.relocation()
                                                                        .hasReached(phase))
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        return writePhase(
                                                                        participant.authorityKey(),
                                                                        snapshot,
                                                                        snapshot.relocation()
                                                                                .phase(phase),
                                                                        cancellation)
                                                                .thenApply(done -> null);
                                                    }));
        }
        return writes;
    }

    static byte[] projectAuthorityPayload(
            byte[] payload,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            long targetGeneration,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkAuthorityGenerationTransition transition) {
        return projectAuthorityPayload(
                payload, targetDescriptor, targetGeneration, targetOwner, transition, null);
    }

    public static byte[] projectAuthorityPayload(
            byte[] application,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            long targetGeneration,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkAuthorityGenerationTransition transition,
            ZLinkAggregateCommitIntent.ActorMembership actorMembership) {
        var actorCodec =
                new systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec();
        var actor = actorCodec.decode(application);
        var spotCodec =
                new systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec();
        var spot = spotCodec.decode(application);
        if (actor.isEmpty() && spot.isEmpty()) {
            if (actorMembership != null) {
                throw new IllegalArgumentException("Relocation Actor authority payload is invalid");
            }
            return application;
        }
        if (transition == ZLinkAuthorityGenerationTransition.PRESERVE) {
            String ownerId =
                    actor.isPresent() ? actor.get().ownerId() : spot.orElseThrow().ownerId();
            long lease =
                    actor.isPresent()
                            ? actor.get().ownerLeaseGeneration()
                            : spot.orElseThrow().ownerLeaseGeneration();
            String mesh =
                    actor.isPresent() ? actor.get().meshName() : spot.orElseThrow().meshName();
            var rid = actor.isPresent() ? actor.get().nodeRid() : spot.orElseThrow().nodeRid();
            long generation =
                    actor.isPresent()
                            ? actor.get().nodeGeneration()
                            : spot.orElseThrow().nodeGeneration();
            if (!ownerId.equals(targetOwner.ownerId())
                    || lease != targetOwner.leaseGeneration()
                    || !mesh.equals(targetDescriptor.meshName())
                    || !rid.equals(targetDescriptor.rid())
                    || generation != targetGeneration)
                throw new IllegalArgumentException(
                        "Preserved authority owner differs from target fence");
            return application;
        }
        if (actor.isPresent()) {
            var value = actor.orElseThrow();
            return actorCodec.encode(
                    value.state(),
                    value.stableType(),
                    value.actorId(),
                    actorMembership == null ? value.currentSpotId() : actorMembership.spotId(),
                    actorMembership == null
                            ? value.currentSpotGeneration()
                            : actorMembership.spotGeneration(),
                    actorMembership == null ? value.currentSpotKind() : actorMembership.spotKind(),
                    targetOwner.ownerId(),
                    targetOwner.leaseGeneration(),
                    targetDescriptor.meshName(),
                    targetDescriptor.rid(),
                    targetGeneration);
        }
        var value = spot.orElseThrow();
        return value.user().isPresent()
                ? spotCodec.encodeUser(
                        value.state(),
                        value.stableType(),
                        value.spotId(),
                        targetOwner.ownerId(),
                        targetOwner.leaseGeneration(),
                        targetDescriptor.meshName(),
                        targetDescriptor.rid(),
                        targetGeneration)
                : spotCodec.encodeInstance(
                        value.state(),
                        value.stableType(),
                        value.spotId(),
                        targetOwner.ownerId(),
                        targetOwner.leaseGeneration(),
                        targetDescriptor.meshName(),
                        targetDescriptor.rid(),
                        targetGeneration,
                        value.activationRecoveryState());
    }

    private CompletionStage<Prepared> prepareAuthority(
            Request request, ZLinkStoreCancellation cancellation) {
        List<ZLinkAggregateParticipant> mutations = new ArrayList<>();
        List<ZLinkAuthoritySnapshot> snapshots = new ArrayList<>();
        CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
        for (Participant participant : canonical(request.participants())) {
            reads =
                    reads.thenCompose(
                            ignored ->
                                    authorityStore
                                            .read(participant.authorityKey(), cancellation)
                                            .thenAccept(
                                                    read -> {
                                                        if (!(read
                                                                instanceof
                                                                ZLinkAuthoritySnapshot snapshot))
                                                            throw new RelocationDataLostException(
                                                                    "Relocation source authority is"
                                                                            + " missing");
                                                        snapshots.add(snapshot);
                                                        mutations.add(
                                                                new ZLinkAggregateParticipant(
                                                                        participant.authorityKey(),
                                                                        participant
                                                                                .objectGeneration(),
                                                                        participant
                                                                                .authorityOwnerGeneration(),
                                                                        participant
                                                                                .expectedStoreVersion(),
                                                                        participant
                                                                                .ownerTransition(),
                                                                        participant
                                                                                .applicationAuthorityPayload(),
                                                                        participant
                                                                                .membershipMutation(),
                                                                        null,
                                                                        participant
                                                                                .casStoreVersion()));
                                                    }));
        }
        return reads.thenCompose(
                ignored -> {
                    String representativeKey = request.representativeKey();
                    int representative =
                            java.util.stream.IntStream.range(0, mutations.size())
                                    .filter(
                                            index ->
                                                    mutations
                                                            .get(index)
                                                            .authorityKey()
                                                            .equals(representativeKey))
                                    .findFirst()
                                    .orElseThrow();
                    var captured =
                            Optional.ofNullable(snapshots.get(representative).relocation())
                                    .filter(
                                            relocation ->
                                                    relocation
                                                            .relocationId()
                                                            .equals(request.aggregateId()));
                    boolean alreadyPrepared =
                            captured.filter(record -> record.phase().equals("prepared"))
                                    .isPresent();
                    byte[] digest =
                            captured.map(
                                            value ->
                                                    HexFormat.of()
                                                            .parseHex(
                                                                    value.sourceInventorySha256()))
                                    .orElseGet(
                                            () -> ZLinkAggregateInventoryStore.digest(mutations));
                    for (int index = 0; index < mutations.size(); index++) {
                        var participant = mutations.get(index);
                        var snapshot = snapshots.get(index);
                        mutations.set(
                                index,
                                new ZLinkAggregateParticipant(
                                        participant.authorityKey(),
                                        participant.objectGeneration(),
                                        participant.sourceAuthorityOwnerGeneration(),
                                        participant.expectedStoreVersion(),
                                        participant.ownerTransition(),
                                        participant.authorityPayload(),
                                        participant.membershipMutation(),
                                        participant.authorityKey().equals(representativeKey)
                                                ? ZLinkAuthorityRelocation.prepared(
                                                        snapshot, request, digest)
                                                : null,
                                        participant.casStoreVersion()));
                    }
                    ZLinkAggregatePrepareRequest storeRequest =
                            new ZLinkAggregatePrepareRequest(
                                    request.aggregateId(),
                                    request.aggregateGeneration(),
                                    mutations,
                                    digest,
                                    request.targetDescriptor(),
                                    request.targetDescriptorLifecycleGeneration(),
                                    request.capacityBundle(),
                                    request.targetOwner(),
                                    representativeKey);
                    ZLinkAggregateFence expectedFence =
                            new ZLinkAggregateFence(
                                    request.aggregateId(), request.aggregateGeneration());
                    CompletionStage<ZLinkAggregatePrepareResult> operation;
                    try {
                        operation = authorityStore.prepareAggregate(storeRequest, cancellation);
                    } catch (RuntimeException failure) {
                        operation = failed(failure);
                    }
                    return operation
                            .handle((result, failure) -> new Attempt<>(result, failure))
                            .thenCompose(
                                    attempt -> {
                                        if (attempt.failure() == null) {
                                            ZLinkAggregateFence fence;
                                            if (attempt.result()
                                                    instanceof ZLinkAggregatePrepared value) {
                                                fence = value.fence();
                                            } else if (attempt.result()
                                                    instanceof
                                                    ZLinkAggregateAlreadyPrepared value) {
                                                fence = value.fence();
                                            } else {
                                                return failed(
                                                        new AuthorityConflictException(
                                                                attempt.result()));
                                            }
                                            return CompletableFuture.completedFuture(
                                                    new Prepared(fence, request, digest));
                                        }
                                        Throwable original = unwrap(attempt.failure());
                                        if (original instanceof ZLinkFrameworkException framework
                                                && framework.kind()
                                                        == ZLinkFrameworkErrorKind.DATA_LOST) {
                                            return failed(original);
                                        }
                                        if (alreadyPrepared) {
                                            return reconcilePreparedReplacement(
                                                    new Prepared(expectedFence, request, digest),
                                                    original);
                                        }
                                        return abortAfterAmbiguousPrepare(expectedFence)
                                                .thenCompose(
                                                        safeToResume ->
                                                                safeToResume
                                                                        ? failed(original)
                                                                        : failed(
                                                                                new PreparationOutcomeUnknownException(
                                                                                        "aggregate"
                                                                                                + " prepare"
                                                                                                + " outcome"
                                                                                                + " could"
                                                                                                + " not be"
                                                                                                + " reconciled",
                                                                                        original,
                                                                                        new Prepared(
                                                                                                expectedFence,
                                                                                                request,
                                                                                                digest))));
                                    });
                });
    }

    private CompletionStage<Prepared> reconcilePreparedReplacement(
            Prepared prepared, Throwable original) {
        var request = prepared.request();
        return authorityStore
                .read(request.representativeKey(), NEVER_CANCELLED)
                .handle(
                        (read, failure) -> {
                            if (failure != null) {
                                var readFailure = unwrap(failure);
                                if (readFailure != original) original.addSuppressed(readFailure);
                                throw new PreparationOutcomeUnknownException(
                                        "Prepared relocation replacement could not be reconciled",
                                        original,
                                        prepared);
                            }
                            if (read instanceof ZLinkAuthoritySnapshot snapshot) {
                                var relocation = snapshot.relocation();
                                if (relocation != null
                                        && relocation.relocationId().equals(request.aggregateId())
                                        && relocation.phase().equals("prepared")
                                        && relocation.targetAttemptGeneration()
                                                == request.targetAttemptGeneration()
                                        && relocation.hasTarget(
                                                request.targetDescriptor().rid(),
                                                request.targetDescriptorLifecycleGeneration(),
                                                request.targetOwner())) {
                                    return prepared;
                                }
                            }
                            throw new CompletionException(original);
                        });
    }

    private CompletionStage<Published> commitAuthority(
            Prepared prepared, ZLinkStoreCancellation cancellation) {
        CompletionStage<ZLinkAggregateCommitResult> operation;
        try {
            operation =
                    authorityStore.commitAggregate(
                            prepared.fence(),
                            new ZLinkAggregateCommitIntent(
                                    prepared.request().representativeKey(),
                                    prepared.request().targetAttemptGeneration(),
                                    prepared.request().actorMembership()),
                            cancellation);
        } catch (RuntimeException failure) {
            operation = failed(failure);
        }
        return operation
                .handle((result, failure) -> new Attempt<>(result, failure))
                .thenCompose(
                        attempt -> {
                            if (attempt.failure() == null
                                    && (attempt.result() == ZLinkAggregateCommitResult.COMMITTED
                                            || attempt.result()
                                                    == ZLinkAggregateCommitResult
                                                            .ALREADY_COMMITTED)) {
                                return publishedFromAggregateMarker(prepared, cancellation);
                            }
                            Throwable original =
                                    attempt.failure() == null
                                            ? new AuthorityConflictException(attempt.result())
                                            : unwrap(attempt.failure());
                            return isPublished(prepared)
                                    .thenCompose(
                                            published ->
                                                    published
                                                            ? publishedFromAggregateMarker(
                                                                    prepared, cancellation)
                                                            : failed(original));
                        });
    }

    private CompletionStage<Published> publishedFromAggregateMarker(
            Prepared prepared, ZLinkStoreCancellation cancellation) {
        return requireAggregateProgress(
                        prepared.fence(),
                        prepared.request().targetOwner(),
                        prepared.inventoryDigest(),
                        cancellation)
                .thenCompose(
                        marker ->
                                readPublishedOwnerGenerations(prepared, cancellation)
                                        .thenApply(
                                                generations ->
                                                        new Published(
                                                                prepared.fence(),
                                                                prepared.request(),
                                                                prepared.inventoryDigest(),
                                                                generations)));
    }

    private CompletionStage<Boolean> isPublished(Prepared prepared) {
        var request = prepared.request();
        return authorityStore
                .read(request.representativeKey(), NEVER_CANCELLED)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                                return CompletableFuture.completedFuture(false);
                            }
                            var relocation = snapshot.relocation();
                            if (relocation == null
                                    || !relocation.hasReached("committed")
                                    || !relocation.relocationId().equals(request.aggregateId())
                                    || relocation.targetAttemptGeneration()
                                            != request.targetAttemptGeneration()) {
                                return CompletableFuture.completedFuture(false);
                            }
                            return authorityStore
                                    .readAggregateProgress(prepared.fence(), NEVER_CANCELLED)
                                    .thenApply(
                                            progress ->
                                                    progress.isPresent()
                                                            && Arrays.equals(
                                                                    progress.orElseThrow()
                                                                            .request()
                                                                            .inventoryDigest(),
                                                                    prepared.inventoryDigest()));
                        })
                .exceptionally(ignored -> false);
    }

    private CompletionStage<Boolean> abortAfterAmbiguousPrepare(ZLinkAggregateFence fence) {
        return authorityStore
                .abortAggregate(fence, NEVER_CANCELLED)
                .handle(
                        (result, failure) ->
                                failure == null
                                        && (result == ZLinkAggregateAbortResult.ABORTED
                                                || result
                                                        == ZLinkAggregateAbortResult
                                                                .ALREADY_ABORTED));
    }

    private static List<Participant> canonical(List<Participant> participants) {
        return participants.stream()
                .sorted(
                        Comparator.comparing(
                                Participant::authorityKey,
                                ZLinkAggregateInventoryStore::compareKeys))
                .toList();
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    public record Participant(
            String authorityKey,
            ZLinkPlacementObjectKind objectKind,
            long objectGeneration,
            long authorityOwnerGeneration,
            String expectedStoreVersion,
            ZLinkAuthorityGenerationTransition ownerTransition,
            byte[] applicationAuthorityPayload,
            byte[] membershipMutation,
            String casStoreVersion) {
        public Participant(
                String authorityKey,
                ZLinkPlacementObjectKind objectKind,
                long objectGeneration,
                long authorityOwnerGeneration,
                String expectedStoreVersion,
                ZLinkAuthorityGenerationTransition ownerTransition,
                byte[] applicationAuthorityPayload,
                byte[] membershipMutation) {
            this(
                    authorityKey,
                    objectKind,
                    objectGeneration,
                    authorityOwnerGeneration,
                    expectedStoreVersion,
                    ownerTransition,
                    applicationAuthorityPayload,
                    membershipMutation,
                    expectedStoreVersion);
        }

        public Participant {
            if (authorityKey == null
                    || authorityKey.isBlank()
                    || expectedStoreVersion == null
                    || expectedStoreVersion.isBlank()) {
                throw new IllegalArgumentException("participant key and StoreVersion are required");
            }
            Objects.requireNonNull(objectKind, "objectKind");
            Objects.requireNonNull(ownerTransition, "ownerTransition");
            if (objectGeneration <= 0 || authorityOwnerGeneration <= 0) {
                throw new IllegalArgumentException("participant generations must be positive");
            }
            applicationAuthorityPayload =
                    Objects.requireNonNull(
                                    applicationAuthorityPayload, "applicationAuthorityPayload")
                            .clone();
            membershipMutation =
                    Objects.requireNonNull(membershipMutation, "membershipMutation").clone();
        }

        @Override
        public byte[] applicationAuthorityPayload() {
            return applicationAuthorityPayload.clone();
        }

        @Override
        public byte[] membershipMutation() {
            return membershipMutation.clone();
        }
    }

    public record Request(
            UUID aggregateId,
            long aggregateGeneration,
            long targetAttemptGeneration,
            List<Participant> participants,
            byte[] root,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            long targetDescriptorLifecycleGeneration,
            ZLinkPlacementCapacityBundle capacityBundle,
            ZLinkLocationOwnerToken targetOwner,
            String coordinatorExpectedStoreVersion,
            ZLinkAggregateCommitIntent.ActorMembership actorMembership) {
        public Request(
                UUID aggregateId,
                long aggregateGeneration,
                long targetAttemptGeneration,
                List<Participant> participants,
                byte[] root,
                ZLinkMeshNodeDescriptorKey targetDescriptor,
                long targetDescriptorLifecycleGeneration,
                ZLinkPlacementCapacityBundle capacityBundle,
                ZLinkLocationOwnerToken targetOwner,
                String coordinatorExpectedStoreVersion) {
            this(
                    aggregateId,
                    aggregateGeneration,
                    targetAttemptGeneration,
                    participants,
                    root,
                    targetDescriptor,
                    targetDescriptorLifecycleGeneration,
                    capacityBundle,
                    targetOwner,
                    coordinatorExpectedStoreVersion,
                    null);
        }

        public Request {
            Objects.requireNonNull(aggregateId, "aggregateId");
            if (aggregateId.equals(new UUID(0, 0))
                    || aggregateGeneration <= 0
                    || aggregateGeneration == Long.MAX_VALUE
                    || targetAttemptGeneration <= 0
                    || targetDescriptorLifecycleGeneration == 0) {
                throw new IllegalArgumentException(
                        "aggregate, attempt, and lifecycle generations are invalid");
            }
            participants = List.copyOf(Objects.requireNonNull(participants, "participants"));
            if (participants.isEmpty()) {
                throw new IllegalArgumentException("participants must contain at least one entry");
            }
            Set<String> keys = new HashSet<>();
            for (Participant participant : participants) {
                if (!keys.add(participant.authorityKey())) {
                    throw new IllegalArgumentException("participant authority keys must be unique");
                }
            }
            root = Objects.requireNonNull(root, "root").clone();
            if (root.length == 0) {
                throw new IllegalArgumentException("root must not be empty");
            }
            Objects.requireNonNull(targetDescriptor, "targetDescriptor");
            Objects.requireNonNull(capacityBundle, "capacityBundle");
            Objects.requireNonNull(targetOwner, "targetOwner");
            Objects.requireNonNull(
                    coordinatorExpectedStoreVersion, "coordinatorExpectedStoreVersion");
            if (targetOwner.ownerId() == null
                    || targetOwner.ownerId().isBlank()
                    || targetOwner.leaseGeneration() <= 0) {
                throw new IllegalArgumentException("target owner token is invalid");
            }
        }

        @Override
        public byte[] root() {
            return root.clone();
        }

        String representativeKey() {
            var object = ZLinkServiceRelocationEnvelopeCodec.decode(root).object();
            return object.kind() == ZLinkPlacementObjectKind.ACTOR.value()
                    ? ZLinkAuthorityKeyCodec.actor(object.objectId())
                    : ZLinkAuthorityKeyCodec.spot(object.objectId());
        }

        /** Framework-issued identity for the immutable canonical root. */
        public String relocationReference() {
            return aggregateId.toString();
        }

        public long relocationChecksumCrc32c() {
            CRC32C checksum = new CRC32C();
            checksum.update(root, 0, root.length);
            return checksum.getValue();
        }
    }

    public record Prepared(ZLinkAggregateFence fence, Request request, byte[] inventoryDigest) {
        public Prepared {
            inventoryDigest = inventoryDigest.clone();
        }

        @Override
        public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }
    }

    public record Published(
            ZLinkAggregateFence fence,
            Request request,
            byte[] inventoryDigest,
            Map<String, Long> targetOwnerGenerations) {
        public Published {
            Objects.requireNonNull(request, "request");
            inventoryDigest = inventoryDigest.clone();
            targetOwnerGenerations = immutableOwnerGenerations(targetOwnerGenerations);
            if (!targetOwnerGenerations
                    .keySet()
                    .equals(
                            request.participants().stream()
                                    .map(Participant::authorityKey)
                                    .collect(Collectors.toSet()))) {
                throw new IllegalArgumentException(
                        "published owner generations do not match participants");
            }
        }

        public long targetOwnerGeneration(String authorityKey) {
            return ownerGeneration(targetOwnerGenerations, authorityKey);
        }

        @Override
        public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }
    }

    /** The prepare and commit results already owned by the target attempt. */
    public record TargetIngressAuthority(
            Supplier<Prepared> prepared, Supplier<Published> committed) {}

    /** Confirms that the exact target attempt owns this route before application admission. */
    public CompletionStage<Boolean> confirmTargetIngress(
            TargetIngressAuthority stage,
            ZLinkAggregateFence fence,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkLocationOwnerToken sourceOwner,
            ZLinkMeshNodeDescriptorKey targetNode,
            long targetNodeGeneration,
            RoutingId sourceNode,
            long sourceNodeGeneration,
            ExpectedParticipant participant,
            long authorityGeneration,
            ZLinkStoreCancellation cancellation) {
        return authorityStore
                .read(participant.authorityKey(), cancellation)
                .thenApply(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                                    || !matchesTargetOwner(snapshot, participant, targetOwner)
                                    || snapshot.allocation().state()
                                            != ZLinkPlacementAllocationState.ACTIVE
                                    || snapshot.authorityOwnerGeneration() != authorityGeneration
                                    || !snapshot.allocation().descriptor().equals(targetNode)
                                    || snapshot.allocation().descriptorLifecycleGeneration()
                                            != targetNodeGeneration) {
                                return false;
                            }
                            return matchesStageAuthority(
                                            stage,
                                            fence,
                                            snapshot,
                                            participant,
                                            targetOwner,
                                            sourceOwner,
                                            sourceNode,
                                            sourceNodeGeneration,
                                            targetNode)
                                    && ownerLeaseValid.test(targetOwner);
                        });
    }

    private static boolean matchesStageAuthority(
            TargetIngressAuthority stage,
            ZLinkAggregateFence fence,
            ZLinkAuthoritySnapshot snapshot,
            ExpectedParticipant participant,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkLocationOwnerToken sourceOwner,
            RoutingId sourceNode,
            long sourceNodeGeneration,
            ZLinkMeshNodeDescriptorKey targetNode) {
        var relocation = snapshot.relocation();
        if (relocation == null) {
            // Normalization removes the relocation proof from Store. The exact commit result
            // identifies the owner generation this stage published.
            var committed = stage.committed().get();
            return committed != null
                    && committed.fence().equals(fence)
                    && Objects.equals(
                            committed.targetOwnerGenerations().get(participant.authorityKey()),
                            snapshot.authorityOwnerGeneration());
        }
        var prepared = stage.prepared().get();
        var publication = publicationForAttempt(snapshot, fence, targetOwner);
        return prepared != null
                && prepared.fence().equals(fence)
                && relocation.hasReached("committed")
                && publication != null
                && publication.targetAttemptGeneration()
                        == prepared.request().targetAttemptGeneration()
                && publication.sourceOwnerId().equals(sourceOwner.ownerId())
                && publication.sourceOwnerLeaseGeneration() == sourceOwner.leaseGeneration()
                && publication.sourceNodeRid().equals(sourceNode)
                && publication.sourceNodeGeneration() == sourceNodeGeneration
                && publication.targetNodeRid().equals(targetNode.rid())
                && java.util.HexFormat.of()
                        .formatHex(prepared.inventoryDigest())
                        .equals(relocation.sourceInventorySha256());
    }

    /** Reads provider-issued owner generations after a target commit. */
    public CompletionStage<Map<String, Long>> readTargetOwnerGenerations(
            List<ExpectedParticipant> participants,
            ZLinkAggregateFence fence,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkStoreCancellation cancellation) {
        return readTargetAuthorities(participants, fence, targetOwner, cancellation)
                .thenApply(
                        snapshots ->
                                snapshots.entrySet().stream()
                                        .collect(
                                                Collectors.toUnmodifiableMap(
                                                        Map.Entry::getKey,
                                                        entry ->
                                                                entry.getValue()
                                                                        .authorityOwnerGeneration())));
    }

    public CompletionStage<Map<String, ZLinkAuthoritySnapshot>> readTargetAuthorities(
            List<ExpectedParticipant> participants,
            ZLinkAggregateFence fence,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkStoreCancellation cancellation) {
        List<ExpectedParticipant> expected =
                List.copyOf(Objects.requireNonNull(participants, "participants"));
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(targetOwner, "targetOwner");
        Objects.requireNonNull(cancellation, "cancellation");
        if (expected.isEmpty()) {
            throw new IllegalArgumentException("target owner generations require participants");
        }
        return requireAggregateProgress(fence, targetOwner, null, cancellation)
                .thenCompose(
                        marker -> {
                            Map<String, ZLinkAuthoritySnapshot> snapshots = new LinkedHashMap<>();
                            CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
                            for (ExpectedParticipant participant : expected) {
                                reads =
                                        reads.thenCompose(
                                                ignored ->
                                                        authorityStore
                                                                .read(
                                                                        participant.authorityKey(),
                                                                        cancellation)
                                                                .thenAccept(
                                                                        read -> {
                                                                            var publication =
                                                                                    targetPublication(
                                                                                            read,
                                                                                            participant,
                                                                                            fence,
                                                                                            targetOwner);
                                                                            if (publication == null
                                                                                    || !publication
                                                                                            .targetNodeRid()
                                                                                            .equals(
                                                                                                    marker.request()
                                                                                                            .targetDescriptor()
                                                                                                            .rid())) {
                                                                                throw new RelocationDataLostException(
                                                                                        "target"
                                                                                                + " owner"
                                                                                                + " generation"
                                                                                                + " differs:"
                                                                                                + " "
                                                                                                + participant
                                                                                                        .authorityKey());
                                                                            }
                                                                            snapshots.put(
                                                                                    participant
                                                                                            .authorityKey(),
                                                                                    (ZLinkAuthoritySnapshot)
                                                                                            read);
                                                                        }));
                            }
                            return reads.thenApply(ignored -> Map.copyOf(snapshots));
                        });
    }

    private static ZLinkCanonicalRelocationAuthorityStateCodec.Published targetPublication(
            ZLinkAuthorityReadResult read,
            ExpectedParticipant participant,
            ZLinkAggregateFence fence,
            ZLinkLocationOwnerToken targetOwner) {
        if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || !matchesTargetOwner(snapshot, participant, targetOwner)) return null;
        return publicationForAttempt(snapshot, fence, targetOwner);
    }

    private static ZLinkCanonicalRelocationAuthorityStateCodec.Published publicationForAttempt(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkAggregateFence fence,
            ZLinkLocationOwnerToken targetOwner) {
        var publication =
                ZLinkAuthorityRelocation.publication(snapshot, fence.aggregateGeneration());
        return publication != null
                        && publication.aggregateId().equals(fence.aggregateId())
                        && publication.aggregateGeneration() == fence.aggregateGeneration()
                        && publication.targetOwnerId().equals(targetOwner.ownerId())
                        && publication.targetOwnerLeaseGeneration() == targetOwner.leaseGeneration()
                        && publication.targetNodeGeneration()
                                == snapshot.allocation().descriptorLifecycleGeneration()
                ? publication
                : null;
    }

    private static boolean matchesTargetOwner(
            ZLinkAuthoritySnapshot snapshot,
            ExpectedParticipant participant,
            ZLinkLocationOwnerToken targetOwner) {
        return snapshot.objectGeneration() == participant.objectGeneration()
                && ownerGenerationAdvanced(
                        participant.sourceAuthorityOwnerGeneration(),
                        snapshot.authorityOwnerGeneration())
                && snapshot.ownerId().equals(targetOwner.ownerId())
                && snapshot.ownerLeaseGeneration() == targetOwner.leaseGeneration();
    }

    private CompletionStage<Map<String, Long>> readPublishedOwnerGenerations(
            Prepared prepared, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(cancellation, "cancellation");
        Map<String, Long> generations = new LinkedHashMap<>();
        CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
        for (Participant participant : prepared.request().participants()) {
            reads =
                    reads.thenCompose(
                            ignored ->
                                    authorityStore
                                            .read(participant.authorityKey(), cancellation)
                                            .thenCompose(
                                                    read -> {
                                                        if (!(read
                                                                        instanceof
                                                                        ZLinkAuthoritySnapshot
                                                                                snapshot)
                                                                || snapshot.objectGeneration()
                                                                        != participant
                                                                                .objectGeneration()
                                                                || !ownerGenerationMatches(
                                                                        participant,
                                                                        snapshot
                                                                                .authorityOwnerGeneration())
                                                                || !snapshot.ownerId()
                                                                        .equals(
                                                                                prepared.request()
                                                                                        .targetOwner()
                                                                                        .ownerId())
                                                                || snapshot.ownerLeaseGeneration()
                                                                        != prepared.request()
                                                                                .targetOwner()
                                                                                .leaseGeneration()) {
                                                            return failed(
                                                                    new RelocationDataLostException(
                                                                            "committed relocation"
                                                                                    + " owner"
                                                                                    + " generation"
                                                                                    + " differs: "
                                                                                    + participant
                                                                                            .authorityKey()));
                                                        }
                                                        var payload =
                                                                ZLinkAuthorityRelocation
                                                                        .publication(
                                                                                snapshot,
                                                                                prepared.fence()
                                                                                        .aggregateGeneration());
                                                        if (payload == null
                                                                || !payload.aggregateId()
                                                                        .equals(
                                                                                prepared.fence()
                                                                                        .aggregateId())
                                                                || payload.aggregateGeneration()
                                                                        != prepared.fence()
                                                                                .aggregateGeneration()
                                                                || payload.targetAttemptGeneration()
                                                                        != prepared.request()
                                                                                .targetAttemptGeneration()
                                                                || !payload.targetOwnerId()
                                                                        .equals(
                                                                                prepared.request()
                                                                                        .targetOwner()
                                                                                        .ownerId())
                                                                || payload
                                                                                .targetOwnerLeaseGeneration()
                                                                        != prepared.request()
                                                                                .targetOwner()
                                                                                .leaseGeneration()) {
                                                            return failed(
                                                                    new RelocationDataLostException(
                                                                            "committed relocation"
                                                                                    + " publication"
                                                                                    + " differs: "
                                                                                    + participant
                                                                                            .authorityKey()));
                                                        }
                                                        generations.put(
                                                                participant.authorityKey(),
                                                                snapshot
                                                                        .authorityOwnerGeneration());
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    }));
        }
        return reads.thenApply(ignored -> Map.copyOf(generations));
    }

    private static boolean ownerGenerationMatches(Participant participant, long actualGeneration) {
        return participant.ownerTransition() == ZLinkAuthorityGenerationTransition.PRESERVE
                ? actualGeneration == participant.authorityOwnerGeneration()
                : ownerGenerationAdvanced(participant.authorityOwnerGeneration(), actualGeneration);
    }

    private static boolean ownerGenerationAdvanced(long sourceGeneration, long targetGeneration) {
        return sourceGeneration != Long.MAX_VALUE && targetGeneration > sourceGeneration;
    }

    private CompletionStage<ZLinkAggregateProgressSnapshot> requireAggregateProgress(
            ZLinkAggregateFence fence,
            ZLinkLocationOwnerToken targetOwner,
            byte[] expectedInventoryDigest,
            ZLinkStoreCancellation cancellation) {
        return authorityStore
                .readAggregateProgress(fence, cancellation)
                .thenCompose(
                        value -> {
                            if (value.isEmpty()) {
                                return failed(
                                        new RelocationDataLostException(
                                                "committed aggregate progress marker is missing"));
                            }
                            ZLinkAggregateProgressSnapshot marker = value.get();
                            boolean valid =
                                    marker.fence().equals(fence)
                                            && marker.request().targetOwner().equals(targetOwner)
                                            && (expectedInventoryDigest == null
                                                    || Arrays.equals(
                                                            marker.request().inventoryDigest(),
                                                            expectedInventoryDigest));
                            return valid
                                    ? CompletableFuture.completedFuture(marker)
                                    : failed(
                                            new RelocationDataLostException(
                                                    "aggregate progress marker fence differs"));
                        });
    }

    private static Map<String, Long> immutableOwnerGenerations(Map<String, Long> generations) {
        Objects.requireNonNull(generations, "targetOwnerGenerations");
        if (generations.isEmpty()
                || generations.entrySet().stream()
                        .anyMatch(
                                entry ->
                                        entry.getKey() == null
                                                || entry.getKey().isBlank()
                                                || entry.getValue() == null
                                                || entry.getValue() <= 0)) {
            throw new IllegalArgumentException("target owner generations are invalid");
        }
        return Map.copyOf(generations);
    }

    private static long ownerGeneration(Map<String, Long> generations, String authorityKey) {
        Long generation = generations.get(Objects.requireNonNull(authorityKey, "authorityKey"));
        if (generation == null) {
            throw new IllegalArgumentException(
                    "authority key is absent from owner generations: " + authorityKey);
        }
        return generation;
    }

    public static final class AuthorityConflictException extends RuntimeException {
        private final Object result;

        public AuthorityConflictException(Object result) {
            super(
                    "aggregate relocation authority operation was rejected: "
                            + (result == null ? "null" : result.getClass().getSimpleName()));
            this.result = result;
        }

        Object result() {
            return result;
        }
    }

    public static final class RelocationDataLostException extends ZLinkFrameworkException {
        public RelocationDataLostException(String message) {
            super(ZLinkFrameworkErrorKind.DATA_LOST, message);
        }
    }

    /** The source seal must remain closed until recovery resolves prepare. */
    public static final class PreparationOutcomeUnknownException extends RuntimeException {
        private final Prepared prepared;

        public PreparationOutcomeUnknownException(
                String message, Throwable cause, Prepared prepared) {
            super(message, cause);
            this.prepared = Objects.requireNonNull(prepared, "prepared");
        }

        public Prepared prepared() {
            return prepared;
        }
    }

    private record Attempt<T>(T result, Throwable failure) {}
}
