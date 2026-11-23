package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;

import java.time.Duration;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Framework-private repository for decoded location and relocation state. */
public interface ZLinkLocationRepository {
    default CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
            ZLinkMeshNodeDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        return updateMeshNode(descriptor, (ZLinkDescriptorWriteIntent) intent);
    }

    CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
            ZLinkMeshNodeDescriptor descriptor, ZLinkDescriptorWriteIntent intent);

    CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
            ZLinkMeshNodeDescriptorKey key, ZLinkLocationOwnerToken owner);

    default CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
            ZLinkMeshNodeDescriptorKey key,
            ZLinkLocationOwnerToken owner,
            ZLinkStoreCancellation cancellation) {
        return removeMeshNode(key, owner);
    }

    CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(
            String meshName, ZLinkPageRequest page);

    /** Refreshes the confirmed metric record for one local descriptor polling cycle. */
    default CompletionStage<Void> refreshCapacityMetrics(ZLinkMeshNodeDescriptorKey local) {
        return CompletableFuture.completedFuture(null);
    }

    CompletionStage<ZLinkLocationWriteResult> updateClientServer(
            ZLinkClientServerServerDescriptor descriptor, ZLinkLocationWriteIntent intent);

    CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
            ZLinkClientServerServerDescriptorKey key, ZLinkLocationOwnerToken owner);

    CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> listClientServers(
            String channelName, ZLinkPageRequest page);

    CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
            ZLinkFanoutPublisherDescriptor descriptor, ZLinkLocationWriteIntent intent);

    CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
            ZLinkFanoutPublisherDescriptorKey key, ZLinkLocationOwnerToken owner);

    CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> listFanoutPublishers(
            String channelName, ZLinkPageRequest page);

    CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(String ownerId, Duration leaseTtl);

    CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId);

    default CompletionStage<Boolean> hasLiveOwner(ZLinkAuthoritySnapshot snapshot) {
        return readOwnerLease(snapshot.ownerId())
                .thenApply(
                        read ->
                                read instanceof ZLinkOwnerLeaseFound found
                                        && found.token().ownerId().equals(snapshot.ownerId())
                                        && found.token().leaseGeneration()
                                                == snapshot.ownerLeaseGeneration()
                                        && found.leaseExpiresAt().isAfter(found.storeNow()));
    }

    CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
            ZLinkLocationOwnerToken token, Duration leaseTtl);

    CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(ZLinkLocationOwnerToken token);

    CompletionStage<ZLinkAuthorityReadResult> read(String key, ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key,
            ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation,
            ZLinkStoreCancellation cancellation);

    /**
     * Cleans one locally discarded incarnation after accepted membership additions have settled.
     * False leaves it for the existing renewal interval; the repository owns Store eligibility.
     */
    default CompletionStage<Boolean> cleanupDiscarded(
            String key,
            ZLinkAuthoritySnapshot discarded,
            ZLinkLocationOwnerToken currentOwner,
            ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
                new UnsupportedOperationException("Discard cleanup is not supported"));
    }

    CompletionStage<ZLinkAuthorityScanResult> list(
            String prefix,
            Optional<ZLinkAuthorityScanCursor> cursor,
            int limit,
            ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request, ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation);

    default CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation, byte[] readyPayload, long deadlineUnixMs) {
        return commit(
                reservation, readyPayload, () -> System.currentTimeMillis() >= deadlineUnixMs);
    }

    default CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            long deadlineUnixMs) {
        return commit(
                reservation,
                readyPayload,
                terminal,
                () -> System.currentTimeMillis() >= deadlineUnixMs);
    }

    CompletionStage<ZLinkObjectRejectResult> reject(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation);

    default CompletionStage<Boolean> releaseEndedReservation(
            String key, String expectedStoreVersion, ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
                new UnsupportedOperationException(
                        "Reservation lifecycle recovery is not supported"));
    }

    /** Policy evidence for Location runtime §6.1, shared by both repository implementations. */
    static void requireDisabledReclaimPolicy(
            ZLinkObjectReservationRequest request, ZLinkMeshNodeDescriptor target) {
        boolean disabled =
                request.usesTargetReclaimPolicy()
                        ? target != null
                                && target.lifecycleGeneration()
                                        == request.targetDescriptorLifecycleGeneration()
                                && target.ownerId().equals(request.targetOwner().ownerId())
                                && target.leaseGeneration()
                                        == request.targetOwner().leaseGeneration()
                                && target.objectCapabilities().stream()
                                        .anyMatch(
                                                capability ->
                                                        capability.objectKind()
                                                                        == request.objectKind()
                                                                && capability
                                                                        .stableType()
                                                                        .equals(
                                                                                request
                                                                                        .stableType())
                                                                && capability.policy()
                                                                        == ZLinkObjectMaintenancePolicyKind
                                                                                .DISABLED)
                        : request.relocationPolicy() instanceof RelocationPolicy.Disabled;
        if (!disabled) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE, "Object owner lease is unavailable");
        }
    }

    default CompletionStage<Boolean> releaseEndedReservation(
            ZLinkObjectReservationRequest request,
            String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
                new UnsupportedOperationException("Ready authority reclamation is not supported"));
    }

    CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation, ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(
            ZLinkCreationOperationIdentity operation, ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation);

    /** Reads the private committed aggregate marker, if one exists. */
    default CompletionStage<Optional<ZLinkAggregateProgressSnapshot>> readAggregateProgress(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
                new UnsupportedOperationException(
                        "aggregate progress is not supported by this repository"));
    }

    /** Releases a marker after participant rows are steady and verified. */
    default CompletionStage<Boolean> removeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
                new UnsupportedOperationException(
                        "aggregate progress is not supported by this repository"));
    }

    CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken owner);

    default CompletionStage<Long> removeAllByOwner(
            ZLinkLocationOwnerToken owner, ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested())
            return CompletableFuture.failedFuture(
                    new java.util.concurrent.TimeoutException("host shutdown deadline expired"));
        return removeAllByOwner(owner);
    }

    default CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
            ZLinkLocationOwnerToken token, ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested())
            return CompletableFuture.failedFuture(
                    new java.util.concurrent.TimeoutException("host shutdown deadline expired"));
        return releaseOwnerLease(token);
    }

    default CompletionStage<OptionalLong> getMeshNodeChangeStamp(String meshName) {
        return CompletableFuture.completedFuture(OptionalLong.empty());
    }
}
