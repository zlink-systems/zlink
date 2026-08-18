package systems.zlink.framework.runtime.internal.locations;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.stream.Collectors;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

/**
 * Publishes all participant routes of one direct-transfer relocation through
 * one bounded Location Store aggregate commit. The handoff payload lives only
 * in source memory and on the wire (spec 28 §4.2) — no store round-trip.
 */
public final class ZLinkAggregateRelocationCoordinator {
    /** Restore operation absolute validity (spec 28 §4.5 retry deadline). */
    private static final Duration RESTORE_VALIDITY = Duration.ofHours(24);
    private static final ZLinkStoreCancellation NEVER_CANCELLED = () -> false;

    private final ZLinkLocationRepository authorityStore;

    public ZLinkAggregateRelocationCoordinator(
        ZLinkLocationRepository authorityStore) {
        this.authorityStore = Objects.requireNonNull(
            authorityStore,
            "authorityStore");
    }

    public CompletionStage<Prepared> prepare(
        Request request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        return prepareAuthority(
            request,
            inventoryDigest(request.participants()),
            cancellation);
    }

    public CompletionStage<Published> commit(
        Prepared prepared,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(prepared, "prepared");
        Objects.requireNonNull(cancellation, "cancellation");
        return commitAuthority(prepared, cancellation);
    }

    public record ExpectedParticipant(
        String authorityKey,
        long objectGeneration,
        long sourceAuthorityOwnerGeneration) {
        public ExpectedParticipant {
            if (authorityKey == null || authorityKey.isBlank()
                || objectGeneration <= 0
                || sourceAuthorityOwnerGeneration <= 0) {
                throw new IllegalArgumentException(
                    "published participant fence is invalid");
            }
        }
    }

    /** Replaces target-published relocation slots with steady authority payloads.
     * Ownership and both object generations are preserved by StoreVersion CAS. */
    public CompletionStage<Void> normalizePublishedAggregate(
        List<ExpectedParticipant> participants,
        ZLinkAggregateFence activatedFence,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkStoreCancellation cancellation) {
        List<ExpectedParticipant> expected = List.copyOf(
            Objects.requireNonNull(participants, "participants"));
        Objects.requireNonNull(activatedFence, "activatedFence");
        Objects.requireNonNull(targetOwner, "targetOwner");
        Objects.requireNonNull(cancellation, "cancellation");
        if (expected.isEmpty()) {
            throw new IllegalArgumentException(
                "completed aggregate participant fence is invalid");
        }
        List<Participant> steady = new ArrayList<>();
        var alreadySteady = new AtomicInteger();
        CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
        for (ExpectedParticipant participant : expected) {
            reads = reads.thenCompose(ignored -> authorityStore.read(
                    participant.authorityKey(), cancellation)
                .thenCompose(read -> collectSteadyParticipant(
                    participant,
                    activatedFence,
                    targetOwner,
                    read,
                    steady,
                    alreadySteady)));
        }
        CompletionStage<Void> participantReads = reads;
        return authorityStore.readAggregateProgress(
                activatedFence, cancellation)
            .thenCompose(marker -> {
            return participantReads.thenCompose(ignored -> {
            if (alreadySteady.get() + steady.size() != expected.size()) {
                return failed(new RelocationDataLostException(
                    "published relocation normalization has an unknown participant state"));
            }
            CompletionStage<Void> writes =
                CompletableFuture.completedFuture(null);
            for (Participant participant : steady) {
                writes = writes.thenCompose(unused ->
                    normalizeParticipant(
                        participant,
                        targetOwner,
                        cancellation));
            }
            return writes.thenCompose(ignoredWrite ->
                removeProgressAfterNormalization(
                    activatedFence,
                    marker,
                    cancellation));
        });
            });
    }

    private CompletionStage<Void> normalizeParticipant(
        Participant participant,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkStoreCancellation cancellation) {
        return authorityStore.compareExchange(
                participant.authorityKey(),
                new ZLinkAuthorityExpectFound(
                    participant.expectedStoreVersion()),
                new ZLinkAuthorityPut(
                    participant.applicationAuthorityPayload()),
                cancellation)
            .thenCompose(result -> {
                if (result instanceof ZLinkAuthorityStored) {
                    return CompletableFuture.completedFuture(null);
                }
                return authorityStore.read(
                        participant.authorityKey(), cancellation)
                    .thenCompose(read -> read instanceof ZLinkAuthoritySnapshot snapshot
                            && isSteadySnapshot(
                                snapshot, participant, targetOwner)
                        ? CompletableFuture.completedFuture(null)
                        : failed(new RelocationDataLostException(
                            "steady authority normalization CAS conflicted: "
                                + participant.authorityKey())));
            });
    }

    private static boolean isSteadySnapshot(
        ZLinkAuthoritySnapshot snapshot,
        Participant participant,
        ZLinkLocationOwnerToken targetOwner) {
        return snapshot.objectGeneration() == participant.objectGeneration()
            && snapshot.authorityOwnerGeneration()
                == participant.authorityOwnerGeneration()
            && snapshot.ownerId().equals(targetOwner.ownerId())
            && snapshot.ownerLeaseGeneration()
                == targetOwner.leaseGeneration()
            && ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                    snapshot.payload()) == null;
    }

    private CompletionStage<Void> removeProgressAfterNormalization(
        ZLinkAggregateFence fence,
        Optional<ZLinkAggregateProgressSnapshot> marker,
        ZLinkStoreCancellation cancellation) {
        if (marker.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        return authorityStore.removeAggregateProgress(
                fence,
                marker.get().storeVersion(),
                cancellation)
            .thenCompose(removed -> {
                if (removed) {
                    return CompletableFuture.completedFuture(null);
                }
                return authorityStore.readAggregateProgress(fence, cancellation)
                    .thenCompose(current -> {
                        if (current.isEmpty()) {
                            return CompletableFuture.completedFuture(null);
                        }
                        return authorityStore.removeAggregateProgress(
                                fence,
                                current.get().storeVersion(),
                                cancellation)
                            .thenCompose(retried -> retried
                                ? CompletableFuture.completedFuture(null)
                                : failed(new RelocationDataLostException(
                                    "aggregate progress removal lost its StoreVersion race")));
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
            return failed(new RelocationDataLostException(
                "completed relocation participant has a different owner fence: "
                    + participant.authorityKey()));
        }
        var publication = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            snapshot.payload());
        if (publication == null) {
            alreadySteady.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
        if (!matchesCompletedParticipant(
            snapshot,
            publication,
            participant,
            activatedFence,
            targetOwner)) {
            return failed(new RelocationDataLostException(
                "completed relocation participant has a different normalization fence: "
                    + participant.authorityKey()));
        }
        steady.add(new Participant(
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
            && publication.aggregateGeneration()
                == activatedFence.aggregateGeneration()
            && publication.targetOwnerId().equals(targetOwner.ownerId())
            && publication.targetOwnerLeaseGeneration()
                == targetOwner.leaseGeneration()
            && publication.targetNodeRid().equals(
                snapshot.allocation().descriptor().rid())
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
        return authorityStore.abortAggregate(
                prepared.fence(),
                NEVER_CANCELLED)
            .thenCompose(result -> {
                if (result != ZLinkAggregateAbortResult.ABORTED
                    && result != ZLinkAggregateAbortResult.ALREADY_ABORTED) {
                    return failed(new IllegalStateException(
                        "aggregate relocation abort was rejected: " + result));
                }
                return CompletableFuture.<Void>completedFuture(null);
            });
    }

    /** Aborts a target-prepared aggregate before cutover without deleting the
     * independently owned immutable source root. */
    public CompletionStage<Void> abortPreparedFence(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(cancellation, "cancellation");
        return authorityStore.abortAggregate(fence, cancellation)
            .thenCompose(result -> result == ZLinkAggregateAbortResult.ABORTED
                    || result == ZLinkAggregateAbortResult.ALREADY_ABORTED
                ? CompletableFuture.completedFuture(null)
                : failed(new AuthorityConflictException(result)));
    }

    private CompletionStage<Prepared> prepareAuthority(
        Request request,
        byte[] digest,
        ZLinkStoreCancellation cancellation) {
        Instant restoreDeadline = Instant.now().plus(RESTORE_VALIDITY);
        List<ZLinkAggregateParticipant> mutations = new ArrayList<>();
        for (Participant participant : canonical(request.participants())) {
            byte[] authorityPayload =
                ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                    participant.applicationAuthorityPayload(),
                    request,
                    participant.ownerTransition());
            mutations.add(new ZLinkAggregateParticipant(
                participant.authorityKey(),
                participant.objectGeneration(),
                participant.authorityOwnerGeneration(),
                participant.expectedStoreVersion(),
                participant.ownerTransition(),
                authorityPayload,
                participant.membershipMutation()));
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
                request.targetOwner());
        ZLinkAggregateFence expectedFence = new ZLinkAggregateFence(
            request.aggregateId(),
            request.aggregateGeneration());
        CompletionStage<ZLinkAggregatePrepareResult> operation;
        try {
            operation = authorityStore.prepareAggregate(storeRequest, cancellation);
        } catch (RuntimeException failure) {
            operation = failed(failure);
        }
        return operation.handle((result, failure) -> new Attempt<>(result, failure))
            .thenCompose(attempt -> {
                if (attempt.failure() == null) {
                    ZLinkAggregateFence fence;
                    if (attempt.result() instanceof ZLinkAggregatePrepared value) {
                        fence = value.fence();
                    } else if (attempt.result()
                        instanceof ZLinkAggregateAlreadyPrepared value) {
                        fence = value.fence();
                    } else {
                        return failed(new AuthorityConflictException(
                            attempt.result()));
                    }
                    return CompletableFuture.completedFuture(new Prepared(
                        fence,
                        request,
                        digest,
                        restoreDeadline));
                }
                Throwable original = unwrap(attempt.failure());
                return abortAfterAmbiguousPrepare(expectedFence)
                    .thenCompose(safeToResume -> safeToResume
                        ? failed(original)
                        : failed(new PreparationOutcomeUnknownException(
                            "aggregate prepare outcome could not be reconciled",
                            original,
                            new Prepared(
                                expectedFence,
                                request,
                                digest,
                                restoreDeadline))));
            });
    }

    private CompletionStage<Published> commitAuthority(
        Prepared prepared,
        ZLinkStoreCancellation cancellation) {
        CompletionStage<ZLinkAggregateCommitResult> operation;
        try {
            operation = authorityStore.commitAggregate(
                prepared.fence(),
                cancellation);
        } catch (RuntimeException failure) {
            operation = failed(failure);
        }
        return operation.handle((result, failure) -> new Attempt<>(result, failure))
            .thenCompose(attempt -> {
                if (attempt.failure() == null
                    && (attempt.result() == ZLinkAggregateCommitResult.COMMITTED
                        || attempt.result()
                            == ZLinkAggregateCommitResult.ALREADY_COMMITTED)) {
                    return publishedFromAggregateMarker(
                        prepared, cancellation);
                }
                Throwable original = attempt.failure() == null
                    ? new AuthorityConflictException(attempt.result())
                    : unwrap(attempt.failure());
                return isPublished(prepared).thenCompose(published -> published
                    ? publishedFromAggregateMarker(prepared, cancellation)
                    : failed(original));
            });
    }

    private CompletionStage<Published> publishedFromAggregateMarker(
        Prepared prepared,
        ZLinkStoreCancellation cancellation) {
        return requireAggregateProgress(
                prepared.fence(),
                prepared.request().targetOwner(),
                prepared.inventoryDigest(),
                cancellation)
            .thenCompose(marker -> readPublishedOwnerGenerations(
                    prepared, cancellation)
                .thenApply(generations -> new Published(
                    prepared.fence(),
                    prepared.request(),
                    prepared.inventoryDigest(),
                    generations)));
    }

    private CompletionStage<Boolean> isPublished(Prepared prepared) {
        return authorityStore.readAggregateProgress(
                prepared.fence(), NEVER_CANCELLED)
            .handle((read, failure) -> failure == null
                && read.isPresent()
                && read.get().request().aggregateId().equals(
                    prepared.fence().aggregateId())
                && read.get().request().aggregateGeneration()
                    == prepared.fence().aggregateGeneration()
                && Arrays.equals(
                    read.get().request().inventoryDigest(),
                    prepared.inventoryDigest()));
    }

    private CompletionStage<Boolean> abortAfterAmbiguousPrepare(
        ZLinkAggregateFence fence) {
        return authorityStore.abortAggregate(fence, NEVER_CANCELLED)
            .handle((result, failure) -> failure == null
                && (result == ZLinkAggregateAbortResult.ABORTED
                    || result == ZLinkAggregateAbortResult.ALREADY_ABORTED));
    }

    private static List<Participant> canonical(List<Participant> participants) {
        return participants.stream()
            .sorted(Comparator.comparing(
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
            return MessageDigest.getInstance("SHA-256")
                .digest(writer.toByteArray());
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static int compareUtf8(String left, String right) {
        return Arrays.compareUnsigned(
            left.getBytes(StandardCharsets.UTF_8),
            right.getBytes(StandardCharsets.UTF_8));
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
            if (authorityKey == null || authorityKey.isBlank()
                || expectedStoreVersion == null
                || expectedStoreVersion.isBlank()) {
                throw new IllegalArgumentException(
                    "participant key and StoreVersion are required");
            }
            Objects.requireNonNull(objectKind, "objectKind");
            Objects.requireNonNull(ownerTransition, "ownerTransition");
            if (objectGeneration <= 0 || authorityOwnerGeneration <= 0) {
                throw new IllegalArgumentException(
                    "participant generations must be positive");
            }
            applicationAuthorityPayload = Objects.requireNonNull(
                applicationAuthorityPayload,
                "applicationAuthorityPayload").clone();
            membershipMutation = Objects.requireNonNull(
                membershipMutation,
                "membershipMutation").clone();
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
        List<Participant> participants,
        byte[] root,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        long targetDescriptorLifecycleGeneration,
        ZLinkPlacementCapacityBundle capacityBundle,
        ZLinkLocationOwnerToken targetOwner) {
        public Request {
            Objects.requireNonNull(aggregateId, "aggregateId");
            if (aggregateId.equals(new UUID(0, 0))
                || aggregateGeneration <= 0
                || targetDescriptorLifecycleGeneration <= 0) {
                throw new IllegalArgumentException(
                    "aggregate and lifecycle generations must be positive");
            }
            participants = List.copyOf(
                Objects.requireNonNull(participants, "participants"));
            if (participants.isEmpty()) {
                throw new IllegalArgumentException(
                    "participants must contain at least one entry");
            }
            Set<String> keys = new HashSet<>();
            for (Participant participant : participants) {
                if (!keys.add(participant.authorityKey())) {
                    throw new IllegalArgumentException(
                        "participant authority keys must be unique");
                }
            }
            root = Objects.requireNonNull(root, "root").clone();
            if (root.length == 0) {
                throw new IllegalArgumentException("root must not be empty");
            }
            Objects.requireNonNull(targetDescriptor, "targetDescriptor");
            Objects.requireNonNull(capacityBundle, "capacityBundle");
            Objects.requireNonNull(targetOwner, "targetOwner");
            if (targetOwner.ownerId() == null
                || targetOwner.ownerId().isBlank()
                || targetOwner.leaseGeneration() <= 0) {
                throw new IllegalArgumentException(
                    "target owner token is invalid");
            }
        }

        @Override
        public byte[] root() {
            return root.clone();
        }
    }

    public record Prepared(
        ZLinkAggregateFence fence,
        Request request,
        byte[] inventoryDigest,
        Instant restoreDeadline) {
        public Prepared {
            inventoryDigest = inventoryDigest.clone();
            Objects.requireNonNull(restoreDeadline, "restoreDeadline");
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
            targetOwnerGenerations = immutableOwnerGenerations(
                targetOwnerGenerations);
            if (!targetOwnerGenerations.keySet().equals(
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

    /** Reads provider-issued owner generations after a target commit. */
    public CompletionStage<Map<String, Long>> readTargetOwnerGenerations(
        List<ExpectedParticipant> participants,
        ZLinkAggregateFence fence,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkStoreCancellation cancellation) {
        List<ExpectedParticipant> expected = List.copyOf(
            Objects.requireNonNull(participants, "participants"));
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(targetOwner, "targetOwner");
        Objects.requireNonNull(cancellation, "cancellation");
        if (expected.isEmpty()) {
            throw new IllegalArgumentException(
                "target owner generations require participants");
        }
        return requireAggregateProgress(
                fence, targetOwner, null, cancellation)
            .thenCompose(marker -> {
            Map<String, Long> generations = new LinkedHashMap<>();
            CompletionStage<Void> reads =
                CompletableFuture.completedFuture(null);
            for (ExpectedParticipant participant : expected) {
                reads = reads.thenCompose(ignored -> authorityStore.read(
                        participant.authorityKey(), cancellation)
                    .thenCompose(read -> {
                    var publication = read instanceof ZLinkAuthoritySnapshot snapshot
                        ? ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                            snapshot.payload())
                        : null;
                    if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                        || snapshot.objectGeneration()
                            != participant.objectGeneration()
                        || !ownerGenerationAdvanced(
                            participant.sourceAuthorityOwnerGeneration(),
                            snapshot.authorityOwnerGeneration())
                        || !snapshot.ownerId().equals(targetOwner.ownerId())
                        || snapshot.ownerLeaseGeneration()
                            != targetOwner.leaseGeneration()
                        || publication == null
                        || !publication.aggregateId().equals(fence.aggregateId())
                        || publication.aggregateGeneration()
                            != fence.aggregateGeneration()
                        || !publication.targetOwnerId().equals(
                            targetOwner.ownerId())
                        || publication.targetOwnerLeaseGeneration()
                            != targetOwner.leaseGeneration()
                        || !publication.targetNodeRid().equals(
                            marker.request().targetDescriptor().rid())
                        || publication.targetNodeGeneration()
                            != snapshot.allocation()
                                .descriptorLifecycleGeneration()) {
                        return failed(new RelocationDataLostException(
                            "target owner generation differs: "
                                + participant.authorityKey()));
                    }
                    generations.put(
                        participant.authorityKey(),
                        snapshot.authorityOwnerGeneration());
                    return CompletableFuture.completedFuture(null);
                    }));
            }
            return reads.thenApply(ignored -> Map.copyOf(generations));
            });
    }

    private CompletionStage<Map<String, Long>> readPublishedOwnerGenerations(
        Prepared prepared,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(cancellation, "cancellation");
        Map<String, Long> generations = new LinkedHashMap<>();
        CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
        for (Participant participant : prepared.request().participants()) {
            reads = reads.thenCompose(ignored -> authorityStore.read(
                    participant.authorityKey(), cancellation)
                .thenCompose(read -> {
                    if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                        || snapshot.objectGeneration()
                            != participant.objectGeneration()
                        || !ownerGenerationMatches(
                            participant, snapshot.authorityOwnerGeneration())
                        || !snapshot.ownerId().equals(
                            prepared.request().targetOwner().ownerId())
                        || snapshot.ownerLeaseGeneration()
                            != prepared.request().targetOwner()
                                .leaseGeneration()) {
                        return failed(new RelocationDataLostException(
                            "committed relocation owner generation differs: "
                                + participant.authorityKey()));
                    }
                    var payload = ZLinkCanonicalRelocationAuthorityStateCodec
                        .decode(snapshot.payload());
                    if (payload == null
                        || !payload.aggregateId().equals(
                            prepared.fence().aggregateId())
                        || payload.aggregateGeneration()
                            != prepared.fence().aggregateGeneration()
                        || !payload.targetOwnerId().equals(
                            prepared.request().targetOwner().ownerId())
                        || payload.targetOwnerLeaseGeneration()
                            != prepared.request().targetOwner()
                                .leaseGeneration()) {
                        return failed(new RelocationDataLostException(
                            "committed relocation publication differs: "
                                + participant.authorityKey()));
                    }
                    generations.put(
                        participant.authorityKey(),
                        snapshot.authorityOwnerGeneration());
                    return CompletableFuture.completedFuture(null);
                }));
        }
        return reads.thenApply(ignored -> Map.copyOf(generations));
    }

    private static boolean ownerGenerationMatches(
        Participant participant,
        long actualGeneration) {
        return participant.ownerTransition()
                == ZLinkAuthorityGenerationTransition.PRESERVE
            ? actualGeneration == participant.authorityOwnerGeneration()
            : ownerGenerationAdvanced(
                participant.authorityOwnerGeneration(), actualGeneration);
    }

    private static boolean ownerGenerationAdvanced(
        long sourceGeneration,
        long targetGeneration) {
        return sourceGeneration != Long.MAX_VALUE
            && targetGeneration > sourceGeneration;
    }

    private CompletionStage<ZLinkAggregateProgressSnapshot>
        requireAggregateProgress(
            ZLinkAggregateFence fence,
            ZLinkLocationOwnerToken targetOwner,
            byte[] expectedInventoryDigest,
            ZLinkStoreCancellation cancellation) {
        return authorityStore.readAggregateProgress(fence, cancellation)
            .thenCompose(value -> {
                if (value.isEmpty()) {
                    return failed(new RelocationDataLostException(
                        "committed aggregate progress marker is missing"));
                }
                ZLinkAggregateProgressSnapshot marker = value.get();
                boolean valid = marker.fence().equals(fence)
                    && marker.request().targetOwner().equals(targetOwner)
                    && (expectedInventoryDigest == null
                        || Arrays.equals(
                            marker.request().inventoryDigest(),
                            expectedInventoryDigest));
                return valid
                    ? CompletableFuture.completedFuture(marker)
                    : failed(new RelocationDataLostException(
                        "aggregate progress marker fence differs"));
            });
    }

    private static Map<String, Long> immutableOwnerGenerations(
        Map<String, Long> generations) {
        Objects.requireNonNull(generations, "targetOwnerGenerations");
        if (generations.isEmpty()
            || generations.entrySet().stream().anyMatch(entry ->
                entry.getKey() == null || entry.getKey().isBlank()
                    || entry.getValue() == null || entry.getValue() <= 0)) {
            throw new IllegalArgumentException(
                "target owner generations are invalid");
        }
        return Map.copyOf(generations);
    }

    private static long ownerGeneration(
        Map<String, Long> generations,
        String authorityKey) {
        Long generation = generations.get(
            Objects.requireNonNull(authorityKey, "authorityKey"));
        if (generation == null) {
            throw new IllegalArgumentException(
                "authority key is absent from owner generations: "
                    + authorityKey);
        }
        return generation;
    }

    public static final class AuthorityConflictException
        extends RuntimeException {
        private final Object result;

        public AuthorityConflictException(Object result) {
            super("aggregate relocation authority operation was rejected: "
                + (result == null ? "null" : result.getClass().getSimpleName()));
            this.result = result;
        }

        Object result() {
            return result;
        }
    }

    public static final class RelocationDataLostException
        extends RuntimeException {
        public RelocationDataLostException(String message) {
            super(message);
        }
    }

    /** The source seal must remain closed until recovery resolves prepare. */
    public static final class PreparationOutcomeUnknownException
        extends RuntimeException {
        private final Prepared prepared;

        public PreparationOutcomeUnknownException(
            String message,
            Throwable cause,
            Prepared prepared) {
            super(message, cause);
            this.prepared = Objects.requireNonNull(prepared, "prepared");
        }

        public Prepared prepared() {
            return prepared;
        }
    }

    private record Attempt<T>(T result, Throwable failure) {
    }

    private static final class Writer {
        private final ByteArrayOutputStream output = new ByteArrayOutputStream();

        void u8(int value) {
            output.write(value);
        }

        void i32(int value) {
            output.writeBytes(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(value).array());
        }

        void i64(long value) {
            output.writeBytes(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
                .putLong(value).array());
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
