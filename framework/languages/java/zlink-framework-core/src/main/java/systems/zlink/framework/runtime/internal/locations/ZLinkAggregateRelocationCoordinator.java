package systems.zlink.framework.runtime.internal.locations;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Duration;
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
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;

/**
 * Prepares an immutable relocation root and publishes all participant routes
 * through one bounded Location Store aggregate commit.
 */
public final class ZLinkAggregateRelocationCoordinator {
    private static final Duration RETENTION = Duration.ofHours(24);
    private static final ZLinkStoreCancellation NEVER_CANCELLED = () -> false;

    private final ZLinkLocationRepository authorityStore;
    private final ZLinkRelocationStore relocationStore;

    public ZLinkAggregateRelocationCoordinator(
        ZLinkLocationRepository authorityStore,
        ZLinkRelocationStore relocationStore) {
        this.authorityStore = Objects.requireNonNull(
            authorityStore,
            "authorityStore");
        this.relocationStore = Objects.requireNonNull(
            relocationStore,
            "relocationStore");
    }

    public CompletionStage<Prepared> prepare(
        Request request,
        ZLinkStoreCancellation cancellation) {
        return stageRoot(request, cancellation).thenCompose(staged ->
            prepareAuthority(
                staged.request(),
                staged.inventoryDigest(),
                staged.stored(),
                cancellation,
                null));
    }

    /** Stores and verifies an initial factory/Restore root without publishing
     * or preparing Location authority. */
    public CompletionStage<StagedRoot> stageRoot(
        Request request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        return stageRoot(
            request,
            inventoryDigest(request.participants()),
            cancellation);
    }

    private CompletionStage<StagedRoot> stageRoot(
        Request request,
        byte[] digest,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        byte[] expectedDigest = Objects.requireNonNull(digest, "digest").clone();
        byte[] root = request.root();
        return ZLinkRelocationTreeStore.put(
                relocationStore,
                root,
                expectedDigest,
                RETENTION,
                cancellation)
            .thenCompose(tree -> ZLinkRelocationTreeStore.read(
                    relocationStore,
                    tree.root().reference(),
                    tree.root().checksumCrc32c(),
                    cancellation)
                .thenCompose(read -> {
                    if (!Arrays.equals(read.logicalRoot(), root)
                        || !Arrays.equals(read.inventoryDigest(), digest)) {
                        return deleteOrphan(tree.root().reference()).thenCompose(
                            ignored -> failed(new RelocationDataLostException(
                                "relocation tree read-back did not preserve the root")));
                    }
                    return CompletableFuture.completedFuture(new StagedRoot(
                        request,
                        tree.root(),
                        expectedDigest));
                }));
    }

    public CompletionStage<Void> discardStagedRoot(StagedRoot staged) {
        Objects.requireNonNull(staged, "staged");
        return deleteOrphan(staged.stored().reference());
    }

    public CompletionStage<Published> commit(
        Prepared prepared,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(prepared, "prepared");
        Objects.requireNonNull(cancellation, "cancellation");
        return ZLinkRelocationTreeStore.renew(
                relocationStore,
                prepared.stored().reference(),
                prepared.stored().checksumCrc32c(),
                RETENTION,
                cancellation)
            .thenCompose(ignored -> commitAuthority(prepared, cancellation));
    }

    /** Reads and verifies the immutable logical root selected by authority. */
    public CompletionStage<Root> readRoot(
        String reference,
        long checksumCrc32c,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(reference, "reference");
        Objects.requireNonNull(cancellation, "cancellation");
        return ZLinkRelocationTreeStore.read(
                relocationStore,
                reference,
                checksumCrc32c,
                cancellation)
            .thenApply(value -> new Root(
                value.logicalRoot(),
                value.inventoryDigest()));
    }

    /**
     * Verifies that Location authority has published the exact immutable root
     * and target owner before a target activation becomes visible.
     */
    public CompletionStage<Void> verifyPublishedRoot(
        String authorityKey,
        ZLinkAggregateFence fence,
        String reference,
        long checksumCrc32c,
        ZLinkLocationOwnerToken targetOwner,
        byte[] inventoryDigest,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(authorityKey, "authorityKey");
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(reference, "reference");
        Objects.requireNonNull(targetOwner, "targetOwner");
        byte[] expectedDigest = Objects.requireNonNull(
            inventoryDigest,
            "inventoryDigest").clone();
        Objects.requireNonNull(cancellation, "cancellation");
        return authorityStore.read(authorityKey, cancellation)
            .thenCompose(read -> {
                if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                    return failed(new RelocationDataLostException(
                        "published relocation authority is missing: "
                            + authorityKey));
                }
                ZLinkCanonicalRelocationAuthorityStateCodec.Published payload =
                    ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                        snapshot.payload());
                if (payload == null
                    || !payload.aggregateId().equals(fence.aggregateId())
                    || payload.aggregateGeneration()
                        != fence.aggregateGeneration()
                    || !payload.reference().equals(reference)
                    || payload.checksumCrc32c() != checksumCrc32c
                    || !payload.targetOwnerId().equals(targetOwner.ownerId())
                    || payload.targetOwnerLeaseGeneration()
                        != targetOwner.leaseGeneration()) {
                    return failed(new RelocationDataLostException(
                        "published relocation authority has a different fence: "
                            + authorityKey));
                }
                return CompletableFuture.completedFuture(null);
            });
    }

    /**
     * Resolves the authority-selected final root and verifies every staged
     * participant before target publication. The initial staging root is not
     * an authority reference and is never accepted here.
     */
    public CompletionStage<PublishedRoot> readPublishedAggregate(
        List<ExpectedParticipant> participants,
        ZLinkAggregateFence fence,
        ZLinkLocationOwnerToken targetOwner,
        byte[] inventoryDigest,
        ZLinkStoreCancellation cancellation) {
        List<ExpectedParticipant> expected = List.copyOf(
            Objects.requireNonNull(participants, "participants"));
        if (expected.isEmpty()) {
            throw new IllegalArgumentException(
                "published aggregate participants are required");
        }
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(targetOwner, "targetOwner");
        byte[] digest = Objects.requireNonNull(
            inventoryDigest, "inventoryDigest").clone();
        Objects.requireNonNull(cancellation, "cancellation");
        return requireAggregateProgress(
                fence, targetOwner, digest, cancellation)
            .thenCompose(marker -> {
                var publication = new java.util.concurrent.atomic.AtomicReference<
                    ZLinkCanonicalRelocationAuthorityStateCodec.Published>();
                Map<String, Long> ownerGenerations = new LinkedHashMap<>();
                CompletionStage<Void> reads =
                    CompletableFuture.completedFuture(null);
                for (ExpectedParticipant participant : expected) {
                    reads = reads.thenCompose(ignored -> authorityStore.read(
                            participant.authorityKey(),
                            cancellation)
                        .thenCompose(read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                                return failed(new RelocationDataLostException(
                                    "published relocation participant is missing: "
                                        + participant.authorityKey()));
                            }
                            var payload =
                                ZLinkCanonicalRelocationAuthorityStateCodec
                                    .decode(snapshot.payload());
                            var first = publication.get();
                            boolean ownerGenerationMatches =
                                ownerGenerationAdvanced(
                                    participant.sourceAuthorityOwnerGeneration(),
                                    snapshot.authorityOwnerGeneration());
                            if (payload == null
                                || snapshot.objectGeneration()
                                    != participant.objectGeneration()
                                || !ownerGenerationMatches
                                || !snapshot.ownerId().equals(
                                    targetOwner.ownerId())
                                || snapshot.ownerLeaseGeneration()
                                    != targetOwner.leaseGeneration()
                                || !payload.aggregateId().equals(
                                    fence.aggregateId())
                                || payload.aggregateGeneration()
                                    != fence.aggregateGeneration()
                                || !payload.targetOwnerId().equals(
                                    targetOwner.ownerId())
                                || payload.targetOwnerLeaseGeneration()
                                    != targetOwner.leaseGeneration()
                                || !payload.targetNodeRid().equals(
                                    marker.request().targetDescriptor().rid())
                                || payload.targetNodeGeneration()
                                    != marker.request()
                                        .targetDescriptorLifecycleGeneration()
                                || first != null
                                    && (!payload.aggregateId().equals(
                                            first.aggregateId())
                                        || payload.aggregateGeneration()
                                            != first.aggregateGeneration())) {
                                return failed(new RelocationDataLostException(
                                    "published relocation participant has a different fence: "
                                        + participant.authorityKey()));
                            }
                            ownerGenerations.put(
                                participant.authorityKey(),
                                snapshot.authorityOwnerGeneration());
                            publication.compareAndSet(null, payload);
                            return CompletableFuture.completedFuture(null);
                        }));
                }
                return reads.thenCompose(ignored -> readRoot(
                        marker.progress().reference(),
                        marker.progress().checksumCrc32c(),
                        cancellation)
                    .thenCompose(root -> Arrays.equals(
                            root.inventoryDigest(), digest)
                        ? CompletableFuture.completedFuture(new PublishedRoot(
                            marker.progress().reference(),
                            marker.progress().checksumCrc32c(),
                            root.payload(),
                            root.inventoryDigest(),
                            ownerGenerations))
                        : failed(new RelocationDataLostException(
                            "published relocation root inventory differs"))));
            });
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

    public record PublishedRoot(
        String reference,
        long checksumCrc32c,
        byte[] payload,
        byte[] inventoryDigest,
        Map<String, Long> targetOwnerGenerations) {
        public PublishedRoot {
            Objects.requireNonNull(reference, "reference");
            payload = Objects.requireNonNull(payload, "payload").clone();
            inventoryDigest = Objects.requireNonNull(
                inventoryDigest, "inventoryDigest").clone();
            targetOwnerGenerations = immutableOwnerGenerations(
                targetOwnerGenerations);
        }

        public long targetOwnerGeneration(String authorityKey) {
            return ownerGeneration(targetOwnerGenerations, authorityKey);
        }

        @Override public byte[] payload() { return payload.clone(); }
        @Override public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }
    }

    /** Verifies the post-cleanup aggregate before target admission opens. */
    public CompletionStage<Void> verifyCompletedAggregate(
        List<ExpectedParticipant> participants,
        ZLinkAggregateFence activatedFence,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkStoreCancellation cancellation) {
        List<ExpectedParticipant> expected = List.copyOf(
            Objects.requireNonNull(participants, "participants"));
        Objects.requireNonNull(activatedFence, "activatedFence");
        Objects.requireNonNull(targetOwner, "targetOwner");
        Objects.requireNonNull(cancellation, "cancellation");
        if (expected.isEmpty()
            || activatedFence.aggregateGeneration() == Long.MAX_VALUE) {
            throw new IllegalArgumentException(
                "completed aggregate participant fence is invalid");
        }
        return requireAggregateProgress(
                activatedFence, targetOwner, null, cancellation)
            .thenCompose(marker -> {
                if (!marker.progress().sourceCleanupCompleted()) {
                    return failed(new RelocationDataLostException(
                        "completed aggregate marker is not in cleanup phase"));
                }
                CompletionStage<Void> reads =
                    CompletableFuture.completedFuture(null);
                for (ExpectedParticipant participant : expected) {
                    reads = reads.thenCompose(ignored -> authorityStore.read(
                            participant.authorityKey(),
                            cancellation)
                        .thenCompose(read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                                return failed(new RelocationDataLostException(
                                    "completed relocation participant is missing: "
                                        + participant.authorityKey()));
                            }
                            var payload =
                                ZLinkCanonicalRelocationAuthorityStateCodec
                                    .decode(snapshot.payload());
                            boolean ownerGenerationMatches =
                                ownerGenerationAdvanced(
                                    participant.sourceAuthorityOwnerGeneration(),
                                    snapshot.authorityOwnerGeneration());
                            if (payload == null
                                || snapshot.objectGeneration()
                                    != participant.objectGeneration()
                                || !ownerGenerationMatches
                                || !snapshot.ownerId().equals(
                                    targetOwner.ownerId())
                                || snapshot.ownerLeaseGeneration()
                                    != targetOwner.leaseGeneration()
                                || !payload.aggregateId().equals(
                                    activatedFence.aggregateId())
                                || payload.aggregateGeneration()
                                    != activatedFence.aggregateGeneration()
                                || !payload.targetOwnerId().equals(
                                    targetOwner.ownerId())
                                || payload.targetOwnerLeaseGeneration()
                                    != targetOwner.leaseGeneration()
                                || !payload.targetNodeRid().equals(
                                    marker.request().targetDescriptor().rid())
                                || payload.targetNodeGeneration()
                                    != marker.request()
                                        .targetDescriptorLifecycleGeneration()) {
                                return failed(new RelocationDataLostException(
                                    "completed relocation participant has a different fence: "
                                        + participant.authorityKey()));
                            }
                            return CompletableFuture.completedFuture(null);
                        }));
                }
                return reads.thenCompose(ignored -> readRoot(
                        marker.progress().reference(),
                        marker.progress().checksumCrc32c(),
                        cancellation)
                    .thenCompose(root -> {
                        var canonical =
                            ZLinkServiceRelocationEnvelopeCodec.decode(
                                root.payload());
                        boolean countsMatch =
                            marker.progress().terminalCompletionCount()
                                == canonical.terminalCompletions().size()
                            && marker.progress().pendingRelayCount()
                                == canonical.pendingRelayCount();
                        return countsMatch
                            && canonical.recoveryReleaseEligible()
                            && canonical.pendingRelayCount() == 0
                            ? CompletableFuture.completedFuture(null)
                            : failed(new RelocationDataLostException(
                                "completed relocation terminal accounting differs"));
                    }));
            });
    }

    /** Replaces completed relocation slots with steady authority payloads.
     * Ownership and both object generations are preserved by StoreVersion CAS. */
    public CompletionStage<Void> normalizeCompletedAggregate(
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
        var alreadySteady = new java.util.concurrent.atomic.AtomicInteger();
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
            if (marker.isPresent()
                && !marker.get().progress().sourceCleanupCompleted()) {
                return failed(new RelocationDataLostException(
                    "aggregate normalization started before cleanup"));
            }
            return participantReads.thenCompose(ignored -> {
            if (alreadySteady.get() + steady.size() != expected.size()) {
                return failed(new RelocationDataLostException(
                    "completed relocation normalization has an unknown participant state"));
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
                    participant.applicationAuthorityPayload(),
                    ZLinkAuthorityGenerationTransition.PRESERVE,
                    java.util.Optional.empty(),
                    java.util.Optional.empty()),
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
        java.util.Optional<ZLinkAggregateProgressSnapshot> marker,
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
                        if (!current.get().progress().sourceCleanupCompleted()) {
                            return failed(new RelocationDataLostException(
                                "aggregate normalization marker regressed before removal"));
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

    /**
     * Stores one immutable replay successor and advances the single private
     * aggregate marker. Participant rows are not used as mutable root
     * pointers after the initial bounded commit.
     */
    public CompletionStage<CanonicalProgress> updateCanonicalReplay(
        List<ExpectedParticipant> participants,
        ZLinkLocationOwnerToken targetOwner,
        java.util.function.UnaryOperator<
            ZLinkServiceRelocationEnvelopeCodec.Envelope> update,
        ZLinkStoreCancellation cancellation) {
        List<ExpectedParticipant> expected = List.copyOf(
            Objects.requireNonNull(participants, "participants"));
        Objects.requireNonNull(targetOwner, "targetOwner");
        Objects.requireNonNull(update, "update");
        Objects.requireNonNull(cancellation, "cancellation");
        if (expected.isEmpty()) {
            throw new IllegalArgumentException(
                "canonical replay participants are required");
        }
        return resolveAggregateFence(expected, targetOwner, cancellation)
            .thenCompose(fence -> updateCanonicalReplay(
                expected,
                fence,
                targetOwner,
                update,
                cancellation));
    }

    /** Internal overload used when the aggregate generation is available. */
    public CompletionStage<CanonicalProgress> updateCanonicalReplay(
        List<ExpectedParticipant> participants,
        ZLinkAggregateFence fence,
        ZLinkLocationOwnerToken targetOwner,
        java.util.function.UnaryOperator<
            ZLinkServiceRelocationEnvelopeCodec.Envelope> update,
        ZLinkStoreCancellation cancellation) {
        List<ExpectedParticipant> expected = List.copyOf(
            Objects.requireNonNull(participants, "participants"));
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(targetOwner, "targetOwner");
        Objects.requireNonNull(update, "update");
        Objects.requireNonNull(cancellation, "cancellation");
        if (expected.isEmpty()) {
            throw new IllegalArgumentException(
                "canonical replay participants are required");
        }
        return requireAggregateProgress(
                fence, targetOwner, null, cancellation)
            .thenCompose(marker -> {
                List<ZLinkAuthoritySnapshot> snapshots = new ArrayList<>();
                var shared = new java.util.concurrent.atomic.AtomicReference<
                    ZLinkCanonicalRelocationAuthorityStateCodec.Published>();
                CompletionStage<Void> reads =
                    CompletableFuture.completedFuture(null);
                for (ExpectedParticipant participant : expected) {
                    reads = reads.thenCompose(ignored -> authorityStore.read(
                            participant.authorityKey(), cancellation)
                        .thenCompose(result -> {
                            if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                                || snapshot.objectGeneration()
                                    != participant.objectGeneration()
                                || !ownerGenerationAdvanced(
                                    participant.sourceAuthorityOwnerGeneration(),
                                    snapshot.authorityOwnerGeneration())
                                || !snapshot.ownerId().equals(
                                    targetOwner.ownerId())
                                || snapshot.ownerLeaseGeneration()
                                    != targetOwner.leaseGeneration()) {
                                return failed(new RelocationDataLostException(
                                    "canonical replay authority owner differs: "
                                        + participant.authorityKey()));
                            }
                            var publication =
                                ZLinkCanonicalRelocationAuthorityStateCodec
                                    .decode(snapshot.payload());
                            var first = shared.get();
                            if (publication == null
                                || !publication.aggregateId().equals(
                                    fence.aggregateId())
                                || publication.aggregateGeneration()
                                    != fence.aggregateGeneration()
                                || !publication.targetOwnerId().equals(
                                    targetOwner.ownerId())
                                || publication.targetOwnerLeaseGeneration()
                                    != targetOwner.leaseGeneration()
                                || !publication.targetNodeRid().equals(
                                    marker.request().targetDescriptor().rid())
                                || publication.targetNodeGeneration()
                                    != marker.request()
                                        .targetDescriptorLifecycleGeneration()
                                || first != null
                                    && (!publication.aggregateId().equals(
                                            first.aggregateId())
                                        || publication.aggregateGeneration()
                                            != first.aggregateGeneration())) {
                                return failed(new RelocationDataLostException(
                                    "canonical replay authority fence differs"));
                            }
                            shared.compareAndSet(null, publication);
                            snapshots.add(snapshot);
                            return CompletableFuture.completedFuture(null);
                        }));
                }
                return reads.thenCompose(ignored -> readRoot(
                        marker.progress().reference(),
                        marker.progress().checksumCrc32c(),
                        cancellation)
                    .thenCompose(stored -> {
                        var current = ZLinkServiceRelocationEnvelopeCodec
                            .decode(stored.payload());
                        var successor = Objects.requireNonNull(
                            update.apply(current),
                            "canonical replay successor");
                        List<String> versions = snapshots.stream()
                            .map(ZLinkAuthoritySnapshot::storeVersion)
                            .toList();
                        List<Long> ownerGenerations = snapshots.stream()
                            .map(ZLinkAuthoritySnapshot::authorityOwnerGeneration)
                            .toList();
                        if (Arrays.equals(
                            successor.canonicalBytes(), current.canonicalBytes())) {
                            return CompletableFuture.completedFuture(
                                new CanonicalProgress(
                                    current, versions, ownerGenerations));
                        }
                        List<Participant> mutations = new ArrayList<>();
                        for (int index = 0; index < expected.size(); index++) {
                            var participant = expected.get(index);
                            var snapshot = snapshots.get(index);
                            mutations.add(new Participant(
                                participant.authorityKey(),
                                snapshot.allocation().objectKind(),
                                snapshot.objectGeneration(),
                                snapshot.authorityOwnerGeneration(),
                                snapshot.storeVersion(),
                                ZLinkAuthorityGenerationTransition.PRESERVE,
                                snapshot.payload(),
                                new byte[0]));
                        }
                        var allocation = snapshots.getFirst().allocation();
                        var request = new Request(
                            fence.aggregateId(),
                            fence.aggregateGeneration(),
                            mutations,
                            successor.canonicalBytes(),
                            marker.request().targetDescriptor(),
                            marker.request()
                                .targetDescriptorLifecycleGeneration(),
                            new ZLinkPlacementCapacityBundle(
                                0, 0, java.util.Optional.empty()),
                            targetOwner);
                        return stageRoot(
                                request,
                                marker.request().inventoryDigest(),
                                cancellation)
                            .thenCompose(staged -> {
                                var nextProgress = new ZLinkAggregateProgress(
                                    staged.stored().reference(),
                                    staged.stored().checksumCrc32c(),
                                    marker.progress().phase(),
                                    marker.progress()
                                        .sourceCleanupCompleted(),
                                    successor.terminalCompletions().size(),
                                    successor.pendingRelayCount());
                                return authorityStore
                                    .compareExchangeAggregateProgress(
                                        fence,
                                        marker.storeVersion(),
                                        nextProgress,
                                        cancellation)
                                    .thenCompose(result -> {
                                        if (result
                                            instanceof ZLinkAggregateProgressStored) {
                                            return CompletableFuture
                                                .completedFuture(
                                                    new CanonicalProgress(
                                                        successor,
                                                        versions,
                                                        ownerGenerations));
                                        }
                                        return authorityStore.readAggregateProgress(
                                                fence, cancellation)
                                            .thenCompose(currentMarker -> {
                                                if (currentMarker.isPresent()
                                                    && currentMarker.get()
                                                        .progress().reference()
                                                        .equals(nextProgress.reference())
                                                    && currentMarker.get()
                                                        .progress()
                                                        .checksumCrc32c()
                                                        == nextProgress
                                                            .checksumCrc32c()) {
                                                    return CompletableFuture
                                                        .completedFuture(
                                                            new CanonicalProgress(
                                                                successor,
                                                                versions,
                                                                ownerGenerations));
                                                }
                                                return deleteOrphan(
                                                        staged.stored().reference())
                                                    .thenCompose(ignoredDelete ->
                                                        failed(
                                                            new RelocationDataLostException(
                                                                "canonical replay marker CAS conflicted")));
                                            });
                                    });
                            });
                    }));
            });
    }

    /** Reads the exact completion published by command 33's authority fence. */
    public CompletionStage<ZLinkServiceRelocationEnvelopeCodec.Completion>
        readCanonicalReply(
            String authorityKey,
            long objectGeneration,
            String sourceOwnerId,
            long sourceOwnerLeaseGeneration,
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceRelocationWireCodec.ReplyRelay relay,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(authorityKey, "authorityKey");
        Objects.requireNonNull(sourceOwnerId, "sourceOwnerId");
        Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
        Objects.requireNonNull(relay, "relay");
        Objects.requireNonNull(cancellation, "cancellation");
        return authorityStore.read(authorityKey, cancellation)
            .thenCompose(result -> {
                if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                    || snapshot.objectGeneration() != objectGeneration
                    || snapshot.authorityOwnerGeneration()
                        != relay.targetAttemptGeneration()
                    || !snapshot.storeVersion().equals(
                        relay.coordinator()
                            .expectedAuthorityStoreVersion())
                    || !snapshot.ownerId().equals(
                        relay.coordinator().ownerId())
                    || snapshot.ownerLeaseGeneration()
                        != relay.coordinator().leaseGeneration()
                    || !snapshot.allocation().descriptor().rid().equals(
                        relay.coordinator().nodeRid())
                    || snapshot.allocation()
                        .descriptorLifecycleGeneration()
                        != relay.coordinator().nodeGeneration()) {
                    return failed(new RelocationDataLostException(
                        "canonical reply authority fence differs"));
                }
                UUID relocation = new UUID(
                    relay.relocation().high(), relay.relocation().low());
                return findAggregateProgress(relocation, cancellation)
                    .thenCompose(marker -> {
                        if (!marker.request().targetOwner().ownerId().equals(
                                relay.coordinator().ownerId())
                            || marker.request().targetOwner()
                                .leaseGeneration()
                                != relay.coordinator().leaseGeneration()
                            || !marker.request().targetDescriptor().rid().equals(
                                relay.coordinator().nodeRid())
                            || marker.request()
                                .targetDescriptorLifecycleGeneration()
                                != relay.coordinator().nodeGeneration()) {
                            return failed(new RelocationDataLostException(
                                "canonical reply aggregate fence differs"));
                        }
                        return readRoot(
                                marker.progress().reference(),
                                marker.progress().checksumCrc32c(),
                                cancellation)
                    .thenCompose(root -> {
                        var envelope =
                            ZLinkServiceRelocationEnvelopeCodec.decode(
                                root.payload());
                        return envelope.terminalCompletions().stream()
                            .filter(value ->
                                value.operationHigh()
                                    == relay.operation().high()
                                && value.operationLow()
                                    == relay.operation().low()
                                && value.sourceOwnerId().equals(sourceOwnerId)
                                && value.sourceOwnerLeaseGeneration()
                                    == sourceOwnerLeaseGeneration
                                && value.sourceNodeRid().equals(
                                    sourceNodeRid.toString())
                                && value.sourceNodeGeneration()
                                    == sourceNodeGeneration
                                && value.participantId()
                                    == relay.participantId()
                                && value.sequence() == relay.sequence()
                                && value.terminalResult()
                                    == relay.terminalResult()
                                && value.failureCode()
                                    == relay.failureCode())
                            .findFirst()
                            .<CompletionStage<ZLinkServiceRelocationEnvelopeCodec.Completion>>map(
                                CompletableFuture::completedFuture)
                            .orElseGet(() -> failed(
                                new RelocationDataLostException(
                                    "canonical reply completion is absent")));
                    });
                    });
            });
    }

    private CompletionStage<Void> collectSteadyParticipant(
        ExpectedParticipant participant,
        ZLinkAggregateFence activatedFence,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkAuthorityReadResult read,
        List<Participant> steady,
        java.util.concurrent.atomic.AtomicInteger alreadySteady) {
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
                return deleteOrphan(prepared.stored().reference());
            });
    }

    public record Root(byte[] payload, byte[] inventoryDigest) {
        public Root {
            payload = Objects.requireNonNull(payload, "payload").clone();
            inventoryDigest = Objects.requireNonNull(
                inventoryDigest,
                "inventoryDigest").clone();
        }

        @Override public byte[] payload() { return payload.clone(); }
        @Override public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }
    }

    public record CanonicalProgress(
        ZLinkServiceRelocationEnvelopeCodec.Envelope root,
        List<String> participantStoreVersions,
        List<Long> participantOwnerGenerations) {
        public CanonicalProgress {
            Objects.requireNonNull(root, "root");
            participantStoreVersions = List.copyOf(
                Objects.requireNonNull(
                    participantStoreVersions,
                    "participantStoreVersions"));
            participantOwnerGenerations = List.copyOf(Objects.requireNonNull(
                participantOwnerGenerations,
                "participantOwnerGenerations"));
            if (participantStoreVersions.isEmpty()) {
                throw new IllegalArgumentException(
                    "canonical progress requires participant versions");
            }
            if (participantOwnerGenerations.size()
                != participantStoreVersions.size()
                || participantOwnerGenerations.stream().anyMatch(
                    value -> value == null || value <= 0)) {
                throw new IllegalArgumentException(
                    "canonical progress owner generations are invalid");
            }
        }

        public String storeVersion(long participantId) {
            if (participantId == 0
                || Long.compareUnsigned(
                    participantId,
                    Integer.toUnsignedLong(participantStoreVersions.size())) > 0) {
                throw new IllegalArgumentException(
                    "participantId is outside the canonical inventory");
            }
            return participantStoreVersions.get((int) participantId - 1);
        }

        public long ownerGeneration(long participantId) {
            if (participantId == 0
                || Long.compareUnsigned(
                    participantId,
                    Integer.toUnsignedLong(
                        participantOwnerGenerations.size())) > 0) {
                throw new IllegalArgumentException(
                    "participantId is outside the canonical inventory");
            }
            return participantOwnerGenerations.get((int) participantId - 1);
        }
    }

    public record StagedRoot(
        Request request,
        ZLinkRelocationStored stored,
        byte[] inventoryDigest) {
        public StagedRoot {
            Objects.requireNonNull(request, "request");
            Objects.requireNonNull(stored, "stored");
            inventoryDigest = Objects.requireNonNull(
                inventoryDigest, "inventoryDigest").clone();
        }

        @Override public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }
    }

    /**
     * Publishes the source-cleanup completion root without changing ownership,
     * generations, membership or capacity. Previous roots remain available to
     * retention-based cleanup after the authority reference changes.
     */
    public CompletionStage<Published> completeSourceCleanup(
        Published published,
        byte[] completedRoot,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(published, "published");
        Objects.requireNonNull(completedRoot, "completedRoot");
        Objects.requireNonNull(cancellation, "cancellation");
        return requireAggregateProgress(
                published.fence(),
                published.request().targetOwner(),
                published.inventoryDigest(),
                cancellation)
            .thenCompose(marker -> {
                List<Participant> completedParticipants = new ArrayList<>();
                CompletionStage<Void> reads =
                    CompletableFuture.completedFuture(null);
                for (Participant original : canonical(
                    published.request().participants())) {
                    reads = reads.thenCompose(ignored -> authorityStore.read(
                            original.authorityKey(),
                            cancellation)
                        .thenCompose(read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                                return failed(new RelocationDataLostException(
                                    "relocation authority disappeared before source cleanup: "
                                        + original.authorityKey()));
                            }
                            var current =
                                ZLinkCanonicalRelocationAuthorityStateCodec
                                    .decode(snapshot.payload());
                            if (current == null
                                || !current.aggregateId().equals(
                                    published.fence().aggregateId())
                                || current.aggregateGeneration()
                                    != published.fence().aggregateGeneration()
                                || !current.targetOwnerId().equals(
                                    published.request().targetOwner().ownerId())
                                || current.targetOwnerLeaseGeneration()
                                    != published.request().targetOwner()
                                        .leaseGeneration()
                                || !current.targetNodeRid().equals(
                                    marker.request().targetDescriptor().rid())
                                || current.targetNodeGeneration()
                                    != marker.request()
                                        .targetDescriptorLifecycleGeneration()
                                || snapshot.objectGeneration()
                                    != original.objectGeneration()
                                || !ownerGenerationAdvanced(
                                    original.authorityOwnerGeneration(),
                                    snapshot.authorityOwnerGeneration())
                                || !snapshot.ownerId().equals(
                                    published.request().targetOwner().ownerId())
                                || snapshot.ownerLeaseGeneration()
                                    != published.request().targetOwner()
                                        .leaseGeneration()) {
                                return failed(new RelocationDataLostException(
                                    "relocation authority has a different completion fence: "
                                        + original.authorityKey()));
                            }
                            completedParticipants.add(new Participant(
                                original.authorityKey(),
                                original.objectKind(),
                                snapshot.objectGeneration(),
                                snapshot.authorityOwnerGeneration(),
                                snapshot.storeVersion(),
                                ZLinkAuthorityGenerationTransition.PRESERVE,
                                snapshot.payload(),
                                new byte[0]));
                            return CompletableFuture.completedFuture(null);
                        }));
                }
                return reads.thenCompose(ignored -> {
                    boolean advanced = !marker.progress().reference().equals(
                            published.stored().reference())
                        || marker.progress().checksumCrc32c()
                            != published.stored().checksumCrc32c();
                    CompletionStage<Root> root = advanced
                        ? readRoot(
                                marker.progress().reference(),
                                marker.progress().checksumCrc32c(),
                                cancellation)
                        : CompletableFuture.completedFuture(new Root(
                            completedRoot,
                            published.inventoryDigest()));
                    return root.thenCompose(authoritative -> {
                        if (!Arrays.equals(
                                authoritative.inventoryDigest(),
                                published.inventoryDigest())) {
                            return failed(new RelocationDataLostException(
                                "source cleanup root inventory digest differs"));
                        }
                        Request completion = new Request(
                            published.request().aggregateId(),
                            published.fence().aggregateGeneration(),
                            completedParticipants,
                            authoritative.payload(),
                            published.request().targetDescriptor(),
                            published.request()
                                .targetDescriptorLifecycleGeneration(),
                            new ZLinkPlacementCapacityBundle(
                                0, 0, java.util.Optional.empty()),
                            published.request().targetOwner());
                        return stageRoot(
                                completion,
                                authoritative.inventoryDigest(),
                                cancellation)
                            .thenCompose(staged -> {
                                var nextProgress = new ZLinkAggregateProgress(
                                    staged.stored().reference(),
                                    staged.stored().checksumCrc32c(),
                                    8,
                                    true,
                                    authoritative.payload() == null
                                        ? 0
                                        : ZLinkServiceRelocationEnvelopeCodec
                                            .decode(authoritative.payload())
                                            .terminalCompletions().size(),
                                    authoritative.payload() == null
                                        ? 0
                                        : ZLinkServiceRelocationEnvelopeCodec
                                            .decode(authoritative.payload())
                                            .pendingRelayCount());
                                return authorityStore
                                    .compareExchangeAggregateProgress(
                                        published.fence(),
                                        marker.storeVersion(),
                                        nextProgress,
                                        cancellation)
                                    .thenCompose(result -> {
                                        CompletionStage<ZLinkAggregateProgressSnapshot>
                                            selected;
                                        if (result
                                            instanceof ZLinkAggregateProgressStored stored) {
                                            selected = CompletableFuture
                                                .completedFuture(stored.snapshot());
                                        } else {
                                            selected = authorityStore
                                                .readAggregateProgress(
                                                    published.fence(),
                                                    cancellation)
                                                .thenCompose(current ->
                                                    current.isPresent()
                                                        && current.get().progress()
                                                            .reference().equals(
                                                                nextProgress.reference())
                                                        && current.get().progress()
                                                            .checksumCrc32c()
                                                            == nextProgress
                                                                .checksumCrc32c()
                                                        ? CompletableFuture
                                                            .completedFuture(
                                                                current.get())
                                                        : failed(
                                                            new RelocationDataLostException(
                                                                "source cleanup marker CAS conflicted")));
                                        }
                                        return selected.thenApply(selectedMarker -> {
                                            Map<String, Long> generations =
                                                new LinkedHashMap<>();
                                            for (Participant participant :
                                                completedParticipants) {
                                                generations.put(
                                                    participant.authorityKey(),
                                                    participant.authorityOwnerGeneration());
                                            }
                                            return new Published(
                                                    published.fence(),
                                                    selectedMarker.progress()
                                                            .reference().equals(
                                                                staged.stored()
                                                                    .reference())
                                                        ? staged.stored()
                                                        : new ZLinkRelocationStored(
                                                            selectedMarker.progress()
                                                                .reference(),
                                                            selectedMarker.progress()
                                                                .checksumCrc32c(),
                                                            published.stored()
                                                                .expiresAt(),
                                                            published.stored()
                                                                .storeNow()),
                                                    completion,
                                                    authoritative.inventoryDigest(),
                                                    generations);
                                        });
                                    });
                            });
                    });
                });
            });
    }

    private CompletionStage<Prepared> prepareAuthority(
        Request request,
        byte[] digest,
        ZLinkRelocationStored stored,
        ZLinkStoreCancellation cancellation) {
        return prepareAuthority(
            request, digest, stored, cancellation, null);
    }

    private CompletionStage<Prepared> prepareAuthority(
        Request request,
        byte[] digest,
        ZLinkRelocationStored stored,
        ZLinkStoreCancellation cancellation,
        Boolean sourceCleanupOverride) {
        List<ZLinkAggregateParticipant> mutations = new ArrayList<>();
        boolean sourceCleanupCompleted = sourceCleanupOverride != null
            ? sourceCleanupOverride
            : request.participants().stream()
                .allMatch(value -> value.ownerTransition()
                    == ZLinkAuthorityGenerationTransition.PRESERVE)
                && request.capacityBundle().actorSlots() == 0
                && request.capacityBundle().spotSlots() == 0;
        for (Participant participant : canonical(request.participants())) {
            byte[] authorityPayload =
                ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                    participant.applicationAuthorityPayload(),
                    request,
                    participant.ownerTransition(),
                    stored,
                    sourceCleanupCompleted);
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
                        return deleteOrphan(stored.reference()).thenCompose(
                            ignored -> failed(new AuthorityConflictException(
                                attempt.result())));
                    }
                    return CompletableFuture.completedFuture(new Prepared(
                        fence,
                        stored,
                        request,
                        digest));
                }
                Throwable original = unwrap(attempt.failure());
                return abortAfterAmbiguousPrepare(
                        expectedFence,
                        stored.reference())
                    .thenCompose(safeToResume -> safeToResume
                        ? failed(original)
                        : failed(new PreparationOutcomeUnknownException(
                            "aggregate prepare outcome could not be reconciled",
                            original)));
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
                .thenApply(generations -> {
                    var progress = marker.progress();
                    ZLinkRelocationStored stored =
                        progress.reference().equals(
                                prepared.stored().reference())
                            && progress.checksumCrc32c()
                                == prepared.stored().checksumCrc32c()
                            ? prepared.stored()
                            : new ZLinkRelocationStored(
                                progress.reference(),
                                progress.checksumCrc32c(),
                                prepared.stored().expiresAt(),
                                prepared.stored().storeNow());
                    return new Published(
                        prepared.fence(),
                        stored,
                        prepared.request(),
                        prepared.inventoryDigest(),
                        generations);
                }));
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
        ZLinkAggregateFence fence,
        String reference) {
        return authorityStore.abortAggregate(fence, NEVER_CANCELLED)
            .handle((result, failure) -> failure == null
                && (result == ZLinkAggregateAbortResult.ABORTED
                    || result == ZLinkAggregateAbortResult.ALREADY_ABORTED))
            .thenCompose(safeToDelete -> safeToDelete
                ? deleteOrphan(reference).thenApply(ignored -> true)
                : CompletableFuture.completedFuture(false));
    }

    private CompletionStage<Void> deleteOrphan(String reference) {
        return ZLinkRelocationTreeStore.delete(
                relocationStore,
                reference,
                NEVER_CANCELLED)
            .handle((ignored, failure) -> null);
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
        ZLinkRelocationStored stored,
        Request request,
        byte[] inventoryDigest) {
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
        ZLinkRelocationStored stored,
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
                    .collect(java.util.stream.Collectors.toSet()))) {
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

    private CompletionStage<ZLinkAggregateProgressSnapshot>
        findAggregateProgress(
            UUID aggregateId,
            ZLinkStoreCancellation cancellation) {
        return authorityStore.listAggregateProgress(cancellation)
            .thenCompose(markers -> {
                List<ZLinkAggregateProgressSnapshot> matches = markers.stream()
                    .filter(value -> value.fence().aggregateId().equals(
                        aggregateId))
                    .toList();
                return matches.size() == 1
                    ? CompletableFuture.completedFuture(matches.getFirst())
                    : failed(new RelocationDataLostException(
                        matches.isEmpty()
                            ? "aggregate progress marker is missing"
                            : "aggregate progress marker is ambiguous"));
            });
    }

    private CompletionStage<ZLinkAggregateFence> resolveAggregateFence(
        List<ExpectedParticipant> participants,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkStoreCancellation cancellation) {
        Set<String> expectedKeys = participants.stream()
            .map(ExpectedParticipant::authorityKey)
            .collect(java.util.stream.Collectors.toSet());
        return authorityStore.listAggregateProgress(cancellation)
            .thenCompose(markers -> {
                List<ZLinkAggregateProgressSnapshot> matches = markers.stream()
                    .filter(value -> value.request().targetOwner().equals(
                        targetOwner))
                    .filter(value -> value.request().participants().stream()
                        .map(ZLinkAggregateParticipant::authorityKey)
                        .collect(java.util.stream.Collectors.toSet())
                        .equals(expectedKeys))
                    .toList();
                return matches.size() == 1
                    ? CompletableFuture.completedFuture(matches.getFirst().fence())
                    : failed(new RelocationDataLostException(
                        matches.isEmpty()
                            ? "aggregate progress marker cannot be resolved"
                            : "aggregate progress marker is ambiguous"));
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
        public PreparationOutcomeUnknownException(
            String message,
            Throwable cause) {
            super(message, cause);
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
