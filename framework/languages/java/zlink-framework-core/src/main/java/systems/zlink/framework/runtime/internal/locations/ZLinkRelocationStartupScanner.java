package systems.zlink.framework.runtime.internal.locations;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

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

    /**
     * Finds direct-Join aggregates whose authority was durably aborted before
     * the matching SOURCE command 44 reached command 45.
     */
    public CompletionStage<List<RetainedSessionAbort>>
        scanRetainedSessionAborts(
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(cancellation, "cancellation");
        return coordinator.resumeTerminalAggregateAbortCleanups(cancellation)
            .thenCompose(ignored ->
                authorityStore.listRetainedAggregateAborts(cancellation))
            .thenCompose(retained -> {
                CompletionStage<List<RetainedSessionAbort>> result =
                    CompletableFuture.completedFuture(new ArrayList<>());
                for (ZLinkAggregateAbortRecoverySnapshot snapshot : retained) {
                    result = result.thenCompose(found ->
                        retainedSessionAbort(snapshot, cancellation)
                            .thenApply(candidate -> {
                                found.add(candidate);
                                return found;
                            }));
                }
                return result.thenApply(found -> found.stream()
                    .sorted(Comparator.comparing(RetainedSessionAbort::reference))
                    .toList());
            });
    }

    private CompletionStage<RetainedSessionAbort> retainedSessionAbort(
        ZLinkAggregateAbortRecoverySnapshot snapshot,
        ZLinkStoreCancellation cancellation) {
        if (snapshot.request().participants().size() != 1) {
            return failedRetainedAbort(
                "retained direct-Join abort inventory is not singular");
        }
        ZLinkAggregateParticipant participant =
            snapshot.request().participants().getFirst();
        var publication = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            participant.authorityPayload());
        if (publication == null) {
            return failedRetainedAbort(
                "retained direct-Join abort has no canonical publication");
        }
        try {
            ZLinkDeferredJoinCompletionAuthority.validateRetainedAbort(
                snapshot,
                publication.reference(),
                publication.checksumCrc32c());
        } catch (RuntimeException invalid) {
            return CompletableFuture.failedFuture(invalid);
        }
        return coordinator.readRoot(
                publication.reference(),
                publication.checksumCrc32c(),
                cancellation)
            .thenCompose(root -> {
                var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(
                    root.payload());
                List<byte[]> commands = ZLinkDeferredJoinCompletionAuthority
                    .retainedSessionRouteCommands(envelope);
                if (!Arrays.equals(
                        root.inventoryDigest(),
                        snapshot.request().inventoryDigest())
                    || envelope.relocationHigh()
                        != snapshot.fence().aggregateId()
                            .getMostSignificantBits()
                    || envelope.relocationLow()
                        != snapshot.fence().aggregateId()
                            .getLeastSignificantBits()
                    || commands.size() != 1) {
                    return failedRetainedAbort(
                        "retained direct-Join abort root inventory differs");
                }
                var intent = new ZLinkServiceM6BWireCodec()
                    .decodeSessionRelocationRouteIntent(commands.getFirst());
                var actor = new ZLinkActorAuthorityPayloadCodec()
                    .decode(publication.applicationPayload())
                    .orElse(null);
                var coordinatorFence = intent.coordinator();
                boolean exact = actor != null
                    && intent.senderRole()
                        == ZLinkServiceM6BWireCodec.RelocationRole.TARGET
                    && intent.action()
                        == ZLinkServiceM6BWireCodec
                            .SessionRelocationRouteAction.COMMIT
                    && intent.relocation().high()
                        == snapshot.fence().aggregateId()
                            .getMostSignificantBits()
                    && intent.relocation().low()
                        == snapshot.fence().aggregateId()
                            .getLeastSignificantBits()
                    && intent.actor().actorId().equals(actor.actorId())
                    && intent.actor().generation()
                        == participant.objectGeneration()
                    && intent.previousAuthorityOwnerGeneration()
                        == participant.sourceAuthorityOwnerGeneration()
                    && coordinatorFence.ownerId().equals(
                        publication.sourceOwnerId())
                    && coordinatorFence.leaseGeneration()
                        == publication.sourceOwnerLeaseGeneration()
                    && coordinatorFence.nodeRid().equals(
                        publication.sourceNodeRid())
                    && coordinatorFence.nodeGeneration()
                        == publication.sourceNodeGeneration()
                    && coordinatorFence.expectedAuthorityStoreVersion().equals(
                        participant.expectedStoreVersion())
                    && intent.targetNodeRid().equals(
                        snapshot.request().targetDescriptor().rid())
                    && intent.targetNodeGeneration()
                        == snapshot.request()
                            .targetDescriptorLifecycleGeneration();
                if (!exact) {
                    return failedRetainedAbort(
                        "retained direct-Join abort route fence differs");
                }
                return CompletableFuture.completedFuture(
                    new RetainedSessionAbort(
                        snapshot,
                        publication.reference(),
                        publication.checksumCrc32c(),
                        snapshot.request().targetDescriptor().meshName(),
                        snapshot.request().targetDescriptor().rid(),
                        snapshot.request()
                            .targetDescriptorLifecycleGeneration(),
                        intent));
            });
    }

    private static <T> CompletionStage<T> failedRetainedAbort(
        String message) {
        return CompletableFuture.failedFuture(
            new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(
                message));
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
                                    != initial.sourceNodeGeneration()
                                || publication.sourceCleanupCompleted()
                                    != initial.sourceCleanupCompleted())) {
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
            return reconcileCompletedMarker(
                    marker,
                    publication,
                    cancellation)
                .thenCompose(completedMarker -> coordinator.readRoot(
                    completedMarker.progress().reference(),
                    completedMarker.progress().checksumCrc32c(),
                    cancellation)
                .whenComplete((root, readFailure) -> {
                    if (readFailure != null) {
                        LOGGER.log(
                            Level.WARNING,
                            () -> "relocation startup root read failed."
                                + " aggregate=" + fence.aggregateId()
                                + " generation=" + fence.aggregateGeneration()
                                + " markerRef="
                                + completedMarker.progress().reference()
                                + " markerCrc="
                                + completedMarker.progress().checksumCrc32c()
                                + " phase="
                                + completedMarker.progress().phase()
                                + " cleanup="
                                + completedMarker.progress()
                                    .sourceCleanupCompleted()
                                + " terminals="
                                + completedMarker.progress()
                                    .terminalCompletionCount()
                                + " pendingRelays="
                                + completedMarker.progress()
                                    .pendingRelayCount()
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
                            completedMarker.progress()
                                    .sourceCleanupCompleted()
                                || publication.sourceCleanupCompleted(),
                            verified,
                            authorities.stream()
                                .sorted(Comparator.comparing(
                                    ZLinkAuthorityEntry::key))
                                .toList())));
                }));
        });
    }

    /**
     * Direct Join writes the participant's Completed publication before the
     * aggregate progress marker. A process can stop between those CAS writes.
     * Startup reconciles only that one monotonic bit after every participant
     * proved the same exact publication fence; no route or high-water is
     * inferred from the stale marker.
     */
    private CompletionStage<ZLinkAggregateProgressSnapshot>
        reconcileCompletedMarker(
            ZLinkAggregateProgressSnapshot marker,
            ZLinkCanonicalRelocationAuthorityStateCodec.Published publication,
            ZLinkStoreCancellation cancellation) {
        if (!publication.sourceCleanupCompleted()
            || marker.progress().sourceCleanupCompleted()) {
            return CompletableFuture.completedFuture(marker);
        }
        var progress = marker.progress();
        var completed = new ZLinkAggregateProgress(
            progress.reference(),
            progress.checksumCrc32c(),
            8,
            true,
            progress.terminalCompletionCount(),
            progress.pendingRelayCount());
        return authorityStore.compareExchangeAggregateProgress(
                marker.fence(),
                marker.storeVersion(),
                completed,
                cancellation)
            .thenCompose(result -> {
                if (result instanceof ZLinkAggregateProgressStored stored) {
                    return CompletableFuture.completedFuture(
                        stored.snapshot());
                }
                return authorityStore.readAggregateProgress(
                        marker.fence(), cancellation)
                    .thenCompose(current -> {
                        if (current.isEmpty()
                            || !sameProgressRoot(
                                marker.progress(),
                                current.get().progress())
                            || !current.get().progress()
                                .sourceCleanupCompleted()) {
                            return failed(
                                "completed relocation marker reconciliation conflicted: "
                                    + marker.fence().aggregateId());
                        }
                        return CompletableFuture.completedFuture(
                            current.get());
                    });
            });
    }

    private static boolean sameProgressRoot(
        ZLinkAggregateProgress expected,
        ZLinkAggregateProgress actual) {
        return expected.reference().equals(actual.reference())
            && expected.checksumCrc32c() == actual.checksumCrc32c()
            && expected.terminalCompletionCount()
                == actual.terminalCompletionCount()
            && expected.pendingRelayCount() == actual.pendingRelayCount();
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

    public record RetainedSessionAbort(
        ZLinkAggregateAbortRecoverySnapshot snapshot,
        String reference,
        long checksumCrc32c,
        String targetMeshName,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        ZLinkServiceM6BWireCodec.SessionRelocationRouteIntent intent) {
        public RetainedSessionAbort {
            Objects.requireNonNull(snapshot, "snapshot");
            Objects.requireNonNull(reference, "reference");
            Objects.requireNonNull(targetMeshName, "targetMeshName");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            Objects.requireNonNull(intent, "intent");
            if (reference.isBlank()
                || targetMeshName.isBlank()
                || targetNodeGeneration <= 0) {
                throw new IllegalArgumentException(
                    "retained Session abort target fence is invalid");
            }
        }
    }
}
