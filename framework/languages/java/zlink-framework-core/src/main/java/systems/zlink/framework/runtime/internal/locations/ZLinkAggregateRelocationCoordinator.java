package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashSet;
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

    public ZLinkAggregateRelocationCoordinator(ZLinkLocationRepository authorityStore) {
        this.authorityStore = Objects.requireNonNull(authorityStore, "authorityStore");
    }

    public CompletionStage<Prepared> prepare(Request request, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        return prepareAuthority(request, inventoryDigest(request.participants()), cancellation);
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
                                                                                "published relocation normalization has"
                                                                                        + " an unknown participant state"));
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

    /**
     * Aborts a target-prepared aggregate before cutover without deleting the independently owned
     * immutable source root.
     */
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

    /** Publishes source capture phases with the authority's current StoreVersion CAS. */
    public CompletionStage<ZLinkAuthoritySnapshot> prepareSource(
            List<ExpectedParticipant> participants,
            String primaryKey,
            java.util.UUID relocationId,
            long applicationVersion,
            ZLinkStoreCancellation cancellation) {
        return sourcePhase(
                participants,
                primaryKey,
                relocationId,
                applicationVersion,
                "preparing",
                cancellation);
    }

    public CompletionStage<ZLinkAuthoritySnapshot> captureSource(
            List<ExpectedParticipant> participants,
            String primaryKey,
            java.util.UUID relocationId,
            long applicationVersion,
            ZLinkStoreCancellation cancellation) {
        return sourcePhase(
                participants,
                primaryKey,
                relocationId,
                applicationVersion,
                "captured",
                cancellation);
    }

    private CompletionStage<ZLinkAuthoritySnapshot> sourcePhase(
            List<ExpectedParticipant> participants,
            String primaryKey,
            java.util.UUID relocationId,
            long applicationVersion,
            String phase,
            ZLinkStoreCancellation cancellation) {
        List<ZLinkAuthorityEntry> entries = new ArrayList<>();
        CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
        for (var participant : participants) {
            reads =
                    reads.thenCompose(
                            ignored ->
                                    authorityStore
                                            .read(participant.authorityKey(), cancellation)
                                            .thenAccept(
                                                    read -> {
                                                        if (!(read
                                                                        instanceof
                                                                        ZLinkAuthoritySnapshot
                                                                                snapshot)
                                                                || snapshot.objectGeneration()
                                                                        != participant
                                                                                .objectGeneration()
                                                                || snapshot
                                                                                .authorityOwnerGeneration()
                                                                        != participant
                                                                                .sourceAuthorityOwnerGeneration())
                                                            throw new IllegalStateException(
                                                                    "Source capture authority fence differs");
                                                        entries.add(
                                                                new ZLinkAuthorityEntry(
                                                                        participant.authorityKey(),
                                                                        snapshot));
                                                    }));
        }
        return reads.thenCompose(
                ignored -> {
                    var primary =
                            entries.stream()
                                    .filter(entry -> entry.key().equals(primaryKey))
                                    .findFirst()
                                    .orElseThrow()
                                    .snapshot();
                    List<Participant> inventory =
                            entries.stream()
                                    .map(
                                            entry ->
                                                    new Participant(
                                                            entry.key(),
                                                            entry.snapshot()
                                                                    .allocation()
                                                                    .objectKind(),
                                                            entry.snapshot().objectGeneration(),
                                                            entry.snapshot()
                                                                    .authorityOwnerGeneration(),
                                                            entry.snapshot().storeVersion(),
                                                            ZLinkAuthorityGenerationTransition
                                                                    .PRESERVE,
                                                            entry.snapshot().payload(),
                                                            new byte[0]))
                                    .toList();
                    String digest = java.util.HexFormat.of().formatHex(inventoryDigest(inventory));
                    CompletionStage<Void> writes = CompletableFuture.completedFuture(null);
                    for (var entry : entries) {
                        writes =
                                writes.thenCompose(
                                        unused -> {
                                            var snapshot = entry.snapshot();
                                            if (!snapshot.ownerId().equals(primary.ownerId())
                                                    || snapshot.ownerLeaseGeneration()
                                                            != primary.ownerLeaseGeneration())
                                                return failed(
                                                        new IllegalStateException(
                                                                "Source capture owner differs"));
                                            var previous = snapshot.relocation();
                                            if (previous != null) {
                                                if (!previous.relocationId().equals(relocationId))
                                                    return failed(
                                                            new IllegalStateException(
                                                                    "Another relocation owns the authority"));
                                                if (previous.hasReached(phase)
                                                        && !previous.phase().equals("aborted"))
                                                    return CompletableFuture.completedFuture(null);
                                                if (!previous.phase().equals("preparing"))
                                                    return failed(
                                                            new IllegalStateException(
                                                                    "Source capture phase differs"));
                                                return writePhase(
                                                                entry.key(),
                                                                snapshot,
                                                                previous.phase("captured"),
                                                                cancellation)
                                                        .thenApply(done -> null);
                                            }
                                            var preparing =
                                                    ZLinkAuthorityRelocation.preparing(
                                                            snapshot,
                                                            primary,
                                                            relocationId,
                                                            digest,
                                                            applicationVersion);
                                            return writePhase(
                                                            entry.key(),
                                                            snapshot,
                                                            preparing,
                                                            cancellation)
                                                    .thenCompose(
                                                            current ->
                                                                    phase.equals("preparing")
                                                                            ? CompletableFuture
                                                                                    .completedFuture(
                                                                                            current)
                                                                            : writePhase(
                                                                                    entry.key(),
                                                                                    current,
                                                                                    preparing.phase(
                                                                                            "captured"),
                                                                                    cancellation))
                                                    .thenApply(done -> null);
                                        });
                    }
                    return writes.thenCompose(
                                    unused -> authorityStore.read(primaryKey, cancellation))
                            .thenApply(read -> (ZLinkAuthoritySnapshot) read);
                });
    }

    public CompletionStage<Void> abortSource(
            List<ExpectedParticipant> participants,
            ZLinkAggregateFence fence,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkStoreCancellation cancellation) {
        return abortPreparedFence(fence, cancellation)
                .thenCompose(
                        ignored -> {
                            return abortSourceProgress(
                                    participants,
                                    fence.aggregateId(),
                                    ownerId,
                                    ownerLeaseGeneration,
                                    cancellation);
                        });
    }

    public CompletionStage<Void> abortSourceProgress(
            List<ExpectedParticipant> participants,
            UUID relocationId,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkStoreCancellation cancellation) {
        return sourceTerminal(
                participants, relocationId, ownerId, ownerLeaseGeneration, "aborted", cancellation);
    }

    public CompletionStage<Void> clearSourceProgress(
            List<ExpectedParticipant> participants,
            UUID relocationId,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkStoreCancellation cancellation) {
        return sourceTerminal(
                participants, relocationId, ownerId, ownerLeaseGeneration, null, cancellation);
    }

    private CompletionStage<Void> sourceTerminal(
            List<ExpectedParticipant> participants,
            UUID relocationId,
            String ownerId,
            long ownerLeaseGeneration,
            String phase,
            ZLinkStoreCancellation cancellation) {
        CompletionStage<Void> writes = CompletableFuture.completedFuture(null);
        for (var participant : participants) {
            writes =
                    writes.thenCompose(
                            unused ->
                                    authorityStore
                                            .read(participant.authorityKey(), cancellation)
                                            .thenCompose(
                                                    read -> {
                                                        if (!(read
                                                                        instanceof
                                                                        ZLinkAuthoritySnapshot
                                                                                snapshot)
                                                                || snapshot.relocation() == null
                                                                || !snapshot.relocation()
                                                                        .relocationId()
                                                                        .equals(relocationId)
                                                                || snapshot.objectGeneration()
                                                                        != participant
                                                                                .objectGeneration()
                                                                || snapshot
                                                                                .authorityOwnerGeneration()
                                                                        != participant
                                                                                .sourceAuthorityOwnerGeneration()
                                                                || !snapshot.ownerId()
                                                                        .equals(ownerId)
                                                                || snapshot.ownerLeaseGeneration()
                                                                        != ownerLeaseGeneration)
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        if (phase == null
                                                                && !snapshot.relocation()
                                                                        .phase()
                                                                        .equals("aborted"))
                                                            return failed(
                                                                    new IllegalStateException(
                                                                            "Source relocation cleanup requires Aborted"));
                                                        return writePhase(
                                                                        participant.authorityKey(),
                                                                        snapshot,
                                                                        phase == null
                                                                                ? null
                                                                                : snapshot.relocation()
                                                                                        .phase(
                                                                                                phase),
                                                                        cancellation)
                                                                .thenApply(done -> null);
                                                    }));
        }
        return writes;
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
                                                                            "Relocation phase authority differs"));
                                                        if (snapshot.relocation() != null
                                                                && snapshot.relocation().aborted()
                                                                && !phase.equals("aborted"))
                                                            return failed(
                                                                    new IllegalStateException(
                                                                            "Relocation was aborted"));
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
            byte[] payload, Request request, ZLinkAuthorityGenerationTransition transition) {
        byte[] application =
                ZLinkCanonicalRelocationAuthorityStateCodec.applicationPayloadOrOriginal(payload);
        var actorCodec =
                new systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec();
        var actor = actorCodec.decode(application);
        var spotCodec =
                new systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec();
        var spot = spotCodec.decode(application);
        if (actor.isEmpty() && spot.isEmpty())
            throw new IllegalArgumentException("Relocation authority payload is invalid");
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
            if (!ownerId.equals(request.targetOwner().ownerId())
                    || lease != request.targetOwner().leaseGeneration()
                    || !mesh.equals(request.targetDescriptor().meshName())
                    || !rid.equals(request.targetDescriptor().rid())
                    || generation != request.targetDescriptorLifecycleGeneration())
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
                    value.currentSpotId(),
                    value.currentSpotGeneration(),
                    value.currentSpotKind(),
                    request.targetOwner().ownerId(),
                    request.targetOwner().leaseGeneration(),
                    request.targetDescriptor().meshName(),
                    request.targetDescriptor().rid(),
                    request.targetDescriptorLifecycleGeneration());
        }
        var value = spot.orElseThrow();
        return value.user().isPresent()
                ? spotCodec.encodeUser(
                        value.state(),
                        value.stableType(),
                        value.spotId(),
                        request.targetOwner().ownerId(),
                        request.targetOwner().leaseGeneration(),
                        request.targetDescriptor().meshName(),
                        request.targetDescriptor().rid(),
                        request.targetDescriptorLifecycleGeneration())
                : spotCodec.encodeInstance(
                        value.state(),
                        value.stableType(),
                        value.spotId(),
                        request.targetOwner().ownerId(),
                        request.targetOwner().leaseGeneration(),
                        request.targetDescriptor().meshName(),
                        request.targetDescriptor().rid(),
                        request.targetDescriptorLifecycleGeneration(),
                        value.activationRecoveryState());
    }

    private CompletionStage<Prepared> prepareAuthority(
            Request request, byte[] digest, ZLinkStoreCancellation cancellation) {
        List<ZLinkAggregateParticipant> mutations = new ArrayList<>();
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
                                                            throw new IllegalStateException(
                                                                    "Relocation source authority is missing");
                                                        byte[] payload =
                                                                projectAuthorityPayload(
                                                                        participant
                                                                                .applicationAuthorityPayload(),
                                                                        request,
                                                                        participant
                                                                                .ownerTransition());
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
                                                                        payload,
                                                                        participant
                                                                                .membershipMutation(),
                                                                        ZLinkAuthorityRelocation
                                                                                .prepared(
                                                                                        snapshot,
                                                                                        request,
                                                                                        digest)));
                                                    }));
        }
        return reads.thenCompose(
                ignored -> {
                    ZLinkAggregatePrepareRequest storeRequest =
                            new ZLinkAggregatePrepareRequest(
                                    request.aggregateId(),
                                    request.aggregateGeneration(),
                                    mutations,
                                    digest,
                                    request.targetDescriptor(),
                                    request.targetDescriptorLifecycleGeneration(),
                                    request.capacityBundle(),
                                    request.targetOwner());
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
                                        return abortAfterAmbiguousPrepare(expectedFence)
                                                .thenCompose(
                                                        safeToResume ->
                                                                safeToResume
                                                                        ? failed(original)
                                                                        : failed(
                                                                                new PreparationOutcomeUnknownException(
                                                                                        "aggregate prepare"
                                                                                                + " outcome could"
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

    private CompletionStage<Published> commitAuthority(
            Prepared prepared, ZLinkStoreCancellation cancellation) {
        CompletionStage<ZLinkAggregateCommitResult> operation;
        try {
            operation = authorityStore.commitAggregate(prepared.fence(), cancellation);
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
        return authorityStore
                .readAggregateProgress(prepared.fence(), NEVER_CANCELLED)
                .handle(
                        (read, failure) ->
                                failure == null
                                        && read.isPresent()
                                        && read.get()
                                                .request()
                                                .aggregateId()
                                                .equals(prepared.fence().aggregateId())
                                        && read.get().request().aggregateGeneration()
                                                == prepared.fence().aggregateGeneration()
                                        && Arrays.equals(
                                                read.get().request().inventoryDigest(),
                                                prepared.inventoryDigest()));
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
                                ZLinkAggregateRelocationCoordinator::compareUtf8))
                .toList();
    }

    private static byte[] inventoryDigest(List<Participant> participants) {
        Writer writer = new Writer();
        for (Participant participant : canonical(participants)) {
            writer.text32(participant.authorityKey());
            writer.u8(participant.objectKind().value());
            writer.i64(participant.objectGeneration());
            writer.i64(participant.authorityOwnerGeneration());
            writer.text32(participant.expectedStoreVersion());
            // Cross-language enum values are Preserve=1 and NewOwner=2.
            writer.u8(participant.ownerTransition().ordinal() + 1);
            writer.bytes32(participant.applicationAuthorityPayload());
            writer.bytes32(participant.membershipMutation());
        }
        try {
            return MessageDigest.getInstance("SHA-256").digest(writer.toByteArray());
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static int compareUtf8(String left, String right) {
        return Arrays.compareUnsigned(
                left.getBytes(StandardCharsets.UTF_8), right.getBytes(StandardCharsets.UTF_8));
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
            byte[] membershipMutation) {
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
            String coordinatorExpectedStoreVersion) {
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

    /** Confirms that the exact target attempt owns this route before application admission. */
    public CompletionStage<Boolean> confirmTargetIngress(
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
                .thenCompose(
                        read -> {
                            var publication =
                                    targetPublication(read, participant, fence, targetOwner);
                            if (publication == null
                                    || !(read instanceof ZLinkAuthoritySnapshot snapshot)
                                    || snapshot.authorityOwnerGeneration() != authorityGeneration
                                    || !snapshot.allocation().descriptor().equals(targetNode)
                                    || snapshot.allocation().descriptorLifecycleGeneration()
                                            != targetNodeGeneration
                                    || snapshot.relocation() == null
                                    || !snapshot.relocation().hasReached("committed")) {
                                return CompletableFuture.completedFuture(false);
                            }
                            if (!publication.sourceOwnerId().equals(sourceOwner.ownerId())
                                    || publication.sourceOwnerLeaseGeneration()
                                            != sourceOwner.leaseGeneration()
                                    || !publication.sourceNodeRid().equals(sourceNode)
                                    || publication.sourceNodeGeneration() != sourceNodeGeneration
                                    || !publication.targetNodeRid().equals(targetNode.rid())
                                    || publication.targetNodeGeneration() != targetNodeGeneration) {
                                return CompletableFuture.completedFuture(false);
                            }
                            return authorityStore
                                    .readAggregateProgress(fence, cancellation)
                                    .thenCompose(
                                            progress -> {
                                                if (progress.isEmpty())
                                                    return CompletableFuture.completedFuture(false);
                                                var marker = progress.orElseThrow();
                                                var request = marker.request();
                                                if (!marker.fence().equals(fence)
                                                        || !request.targetOwner()
                                                                .equals(targetOwner)
                                                        || !request.targetDescriptor()
                                                                .equals(targetNode)
                                                        || request
                                                                        .targetDescriptorLifecycleGeneration()
                                                                != targetNodeGeneration
                                                        || !java.util.HexFormat.of()
                                                                .formatHex(
                                                                        request.inventoryDigest())
                                                                .equals(
                                                                        snapshot.relocation()
                                                                                .sourceInventorySha256())) {
                                                    return CompletableFuture.completedFuture(false);
                                                }
                                                return authorityStore
                                                        .readOwnerLease(targetOwner.ownerId())
                                                        .thenApply(
                                                                lease ->
                                                                        lease
                                                                                        instanceof
                                                                                        ZLinkOwnerLeaseFound
                                                                                                found
                                                                                && found.isLiveFor(
                                                                                        targetOwner
                                                                                                .ownerId(),
                                                                                        targetOwner
                                                                                                .leaseGeneration()));
                                            });
                        });
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
                                                                                        "target owner generation differs: "
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
                || snapshot.objectGeneration() != participant.objectGeneration()
                || !ownerGenerationAdvanced(
                        participant.sourceAuthorityOwnerGeneration(),
                        snapshot.authorityOwnerGeneration())
                || !snapshot.ownerId().equals(targetOwner.ownerId())
                || snapshot.ownerLeaseGeneration() != targetOwner.leaseGeneration()) return null;
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

    public static final class RelocationDataLostException extends RuntimeException {
        public RelocationDataLostException(String message) {
            super(message);
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

    private static final class Writer {
        private final ByteArrayOutputStream output = new ByteArrayOutputStream();

        void u8(int value) {
            output.write(value);
        }

        void i32(int value) {
            output.writeBytes(
                    ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array());
        }

        void i64(long value) {
            output.writeBytes(
                    ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(value).array());
        }

        void text32(String value) {
            bytes32(value.getBytes(StandardCharsets.UTF_8));
        }

        void bytes32(byte[] value) {
            i32(value.length);
            output.writeBytes(value);
        }

        byte[] toByteArray() {
            return output.toByteArray();
        }
    }
}
