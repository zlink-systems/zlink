package systems.zlink.framework.runtime.internal.locations;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/**
 * Finds committed aggregate markers after a runtime restart. The marker is
 * the source of the mutable root pointer; participant rows are checked only
 * for their owner, object and immutable aggregate fence.
 */
public final class ZLinkRelocationStartupScanner {
    private final ZLinkLocationRepository authorityStore;
    private final ZLinkAggregateRelocationCoordinator coordinator;

    public ZLinkRelocationStartupScanner(
        ZLinkLocationRepository authorityStore,
        ZLinkRelocationStore relocationStore) {
        this.authorityStore = Objects.requireNonNull(
            authorityStore, "authorityStore");
        this.coordinator = new ZLinkAggregateRelocationCoordinator(
            authorityStore,
            Objects.requireNonNull(relocationStore, "relocationStore"));
    }

    public CompletionStage<List<Candidate>> scan(
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(cancellation, "cancellation");
        return authorityStore.listAggregateProgress(cancellation)
            .thenCompose(markers -> {
                CompletionStage<List<Candidate>> result =
                    CompletableFuture.completedFuture(new ArrayList<>());
                for (ZLinkAggregateProgressSnapshot marker : markers) {
                    result = result.thenCompose(found ->
                        verifyMarker(marker, cancellation).thenApply(candidate -> {
                            found.add(candidate);
                            return found;
                        }));
                }
                return result.thenApply(found -> found.stream()
                    .sorted(Comparator.comparing(Candidate::reference))
                    .toList());
            });
    }

    private CompletionStage<Candidate> verifyMarker(
        ZLinkAggregateProgressSnapshot marker,
        ZLinkStoreCancellation cancellation) {
        ZLinkAggregateFence fence = marker.fence();
        ZLinkLocationOwnerToken target = marker.request().targetOwner();
        List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant> expected =
            new ArrayList<>();
        List<ZLinkAuthorityEntry> authorities = new ArrayList<>();
        var first = new java.util.concurrent.atomic.AtomicReference<
            ZLinkCanonicalRelocationAuthorityStateCodec.Published>();
        CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
        for (ZLinkAggregateParticipant participant :
            marker.request().participants()) {
            reads = reads.thenCompose(ignored -> authorityStore.read(
                    participant.authorityKey(), cancellation)
                .thenCompose(read -> {
                    if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                        return failed("published relocation participant is missing: "
                            + participant.authorityKey());
                    }
                    var publication =
                        ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                            snapshot.payload());
                    var initial = first.get();
                    boolean ownerGenerationMatches =
                        participant.ownerTransition()
                            == ZLinkAuthorityGenerationTransition.PRESERVE
                            ? snapshot.authorityOwnerGeneration()
                                == participant.sourceAuthorityOwnerGeneration()
                            : participant.sourceAuthorityOwnerGeneration()
                                != Long.MAX_VALUE
                                && snapshot.authorityOwnerGeneration()
                                    > participant.sourceAuthorityOwnerGeneration();
                    if (publication == null
                        || snapshot.objectGeneration()
                            != participant.objectGeneration()
                        || !ownerGenerationMatches
                        || !snapshot.ownerId().equals(target.ownerId())
                        || snapshot.ownerLeaseGeneration()
                            != target.leaseGeneration()
                        || !publication.aggregateId().equals(
                            fence.aggregateId())
                        || publication.aggregateGeneration()
                            != fence.aggregateGeneration()
                        || !publication.targetOwnerId().equals(
                            target.ownerId())
                        || publication.targetOwnerLeaseGeneration()
                            != target.leaseGeneration()
                        || !publication.targetNodeRid().equals(
                            marker.request().targetDescriptor().rid())
                        || publication.targetNodeGeneration()
                            != marker.request()
                                .targetDescriptorLifecycleGeneration()
                        || initial != null
                            && (!publication.sourceOwnerId().equals(
                                    initial.sourceOwnerId())
                                || publication.sourceOwnerLeaseGeneration()
                                    != initial.sourceOwnerLeaseGeneration()
                                || !publication.sourceNodeRid().equals(
                                    initial.sourceNodeRid())
                                || publication.sourceNodeGeneration()
                                    != initial.sourceNodeGeneration())) {
                        return failed(
                            "published relocation participant fence differs: "
                                + participant.authorityKey());
                    }
                    first.compareAndSet(null, publication);
                    authorities.add(new ZLinkAuthorityEntry(
                        participant.authorityKey(), snapshot));
                    expected.add(
                        new ZLinkAggregateRelocationCoordinator
                            .ExpectedParticipant(
                            participant.authorityKey(),
                            participant.objectGeneration(),
                            participant.sourceAuthorityOwnerGeneration()));
                    return CompletableFuture.completedFuture(null);
                }));
        }
        return reads.thenCompose(ignored -> {
            var publication = first.get();
            return coordinator.readRoot(
                    marker.progress().reference(),
                    marker.progress().checksumCrc32c(),
                    cancellation)
                .thenCompose(root -> {
                    var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(
                        root.payload());
                    if (!java.util.Arrays.equals(
                            root.inventoryDigest(),
                            marker.request().inventoryDigest())
                        || envelope.relocationHigh()
                            != fence.aggregateId().getMostSignificantBits()
                        || envelope.relocationLow()
                            != fence.aggregateId().getLeastSignificantBits()
                        || envelope.participantProgress().size()
                            != marker.request().participants().size()
                        || envelope.applicationStates().size()
                            != marker.request().participants().size()) {
                        return failed(
                            "published relocation root inventory is incomplete");
                    }
                    return coordinator.readPublishedAggregate(
                            expected,
                            fence,
                            target,
                            marker.request().inventoryDigest(),
                            cancellation)
                        .thenApply(verified -> new Candidate(
                            marker.progress().reference(),
                            marker.progress().checksumCrc32c(),
                            fence,
                            publication.sourceOwnerId(),
                            publication.sourceOwnerLeaseGeneration(),
                            publication.sourceNodeRid(),
                            publication.sourceNodeGeneration(),
                            target,
                            marker.request().targetDescriptor().rid(),
                            marker.request()
                                .targetDescriptorLifecycleGeneration(),
                            marker.progress().sourceCleanupCompleted(),
                            verified,
                            authorities.stream()
                                .sorted(Comparator.comparing(
                                    ZLinkAuthorityEntry::key))
                                .toList()));
                });
        });
    }

    private static <T> CompletionStage<T> failed(String message) {
        return CompletableFuture.failedFuture(
            new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(
                message));
    }

    public record Candidate(
        String reference,
        long checksumCrc32c,
        ZLinkAggregateFence fence,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        systems.zlink.contracts.core.RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkLocationOwnerToken targetOwner,
        systems.zlink.contracts.core.RoutingId targetNodeRid,
        long targetNodeGeneration,
        boolean sourceCleanupCompleted,
        ZLinkAggregateRelocationCoordinator.PublishedRoot root,
        List<ZLinkAuthorityEntry> authorities) {
        public Candidate {
            Objects.requireNonNull(reference, "reference");
            Objects.requireNonNull(fence, "fence");
            Objects.requireNonNull(sourceOwnerId, "sourceOwnerId");
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            Objects.requireNonNull(targetOwner, "targetOwner");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            Objects.requireNonNull(root, "root");
            authorities = List.copyOf(authorities);
            if (reference.isBlank()
                || authorities.isEmpty()
                || sourceOwnerId.isBlank()
                || sourceOwnerLeaseGeneration <= 0
                || sourceNodeGeneration <= 0
                || targetNodeGeneration <= 0) {
                throw new IllegalArgumentException(
                    "relocation recovery owner and node fences are invalid");
            }
        }
    }
}
