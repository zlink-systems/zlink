package systems.zlink.framework.runtime.internal.locations;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.core.RoutingId;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Finds committed aggregate markers after a runtime restart. The marker is
 * the source of the mutable root pointer; participant rows are checked only
 * for their owner, object and immutable aggregate fence.
 *
 * <p>A marker whose participants no longer publish a relocation slot is not
 * data loss. Normalization writes the steady participant payloads first and
 * removes the marker afterwards, and a participant row deleted after the move
 * leaves the same orphan, so a process that stops in either window leaves a
 * marker no authority points through. The scan finishes that removal instead of
 * failing every later start in the same Location scope. A participant that is
 * still published but disagrees with the marker, and a set that is only partly
 * published, remain unrecoverable failures.</p>
 */
public final class ZLinkRelocationStartupScanner {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkRelocationStartupScanner.class.getName());
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
                            candidate.ifPresent(found::add);
                            return found;
                        }));
                }
                return result.thenApply(found -> found.stream()
                    .sorted(Comparator.comparing(Candidate::reference))
                    .toList());
            });
    }

    private CompletionStage<Optional<Candidate>> verifyMarker(
        ZLinkAggregateProgressSnapshot marker,
        ZLinkStoreCancellation cancellation) {
        ZLinkAggregateFence fence = marker.fence();
        ZLinkLocationOwnerToken target = marker.request().targetOwner();
        List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant> expected =
            new ArrayList<>();
        List<ZLinkAuthorityEntry> authorities = new ArrayList<>();
        List<String> settled = new ArrayList<>();
        List<String> pointers = new ArrayList<>();
        var first = new AtomicReference<
            ZLinkCanonicalRelocationAuthorityStateCodec.Published>();
        CompletionStage<Void> reads = CompletableFuture.completedFuture(null);
        for (ZLinkAggregateParticipant participant :
            marker.request().participants()) {
            reads = reads.thenCompose(ignored -> authorityStore.read(
                    participant.authorityKey(), cancellation)
                .thenCompose(read -> {
                    if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                        // The row this marker points at is gone. Nothing
                        // published references the root through it any more.
                        settled.add(participant.authorityKey() + "=absent");
                        return CompletableFuture.completedFuture(null);
                    }
                    var publication =
                        ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                            snapshot.payload());
                    if (publication == null) {
                        // The row carries a steady application payload, so it
                        // no longer points at the relocation root, whether
                        // normalization wrote it or a later activation did.
                        settled.add(participant.authorityKey() + "=steady");
                        return CompletableFuture.completedFuture(null);
                    }
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
                    if (snapshot.objectGeneration()
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
                    pointers.add(participant.authorityKey()
                        + "->" + publication.reference()
                        + "/" + publication.checksumCrc32c()
                        + "/cleanup=" + publication.sourceCleanupCompleted());
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
            int total = marker.request().participants().size();
            if (settled.size() == total) {
                return coordinator.discardOrphanedAggregateProgress(
                        marker, cancellation)
                    .thenApply(removed -> {
                        LOGGER.log(
                            Level.WARNING,
                            () -> "orphaned relocation progress marker "
                                + (removed ? "removed" : "skipped")
                                + ". aggregate=" + fence.aggregateId()
                                + " generation=" + fence.aggregateGeneration()
                                + " participants=" + settled
                                + " sourceCleanupCompleted="
                                + marker.progress().sourceCleanupCompleted());
                        return Optional.<Candidate>empty();
                    });
            }
            if (!settled.isEmpty()) {
                return ZLinkRelocationStartupScanner.<Optional<Candidate>>failed(
                    "published relocation participants are partially "
                        + "normalized: " + settled + " of " + total
                        + " for aggregate " + fence.aggregateId());
            }
            var publication = first.get();
            return coordinator.readRoot(
                    marker.progress().reference(),
                    marker.progress().checksumCrc32c(),
                    cancellation)
                .whenComplete((root, readFailure) -> {
                    if (readFailure != null) {
                        LOGGER.log(
                            Level.WARNING,
                            () -> "relocation startup root read failed."
                                + " aggregate=" + fence.aggregateId()
                                + " generation=" + fence.aggregateGeneration()
                                + " markerRef=" + marker.progress().reference()
                                + " markerCrc="
                                + marker.progress().checksumCrc32c()
                                + " phase=" + marker.progress().phase()
                                + " cleanup="
                                + marker.progress().sourceCleanupCompleted()
                                + " terminals="
                                + marker.progress().terminalCompletionCount()
                                + " pendingRelays="
                                + marker.progress().pendingRelayCount()
                                + " targetOwner=" + target.ownerId()
                                + "/" + target.leaseGeneration()
                                + " targetNode="
                                + marker.request().targetDescriptor().rid()
                                + "/" + marker.request()
                                    .targetDescriptorLifecycleGeneration()
                                + " published=" + pointers
                                + " cause=" + readFailure);
                    }
                })
                .thenCompose(root -> {
                    var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(
                        root.payload());
                    if (!Arrays.equals(
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
                        return ZLinkRelocationStartupScanner
                            .<Optional<Candidate>>failed(
                                "published relocation root inventory is incomplete");
                    }
                    return coordinator.readPublishedAggregate(
                            expected,
                            fence,
                            target,
                            marker.request().inventoryDigest(),
                            cancellation)
                        .thenApply(verified -> Optional.of(new Candidate(
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
                                .toList())));
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
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkLocationOwnerToken targetOwner,
        RoutingId targetNodeRid,
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
