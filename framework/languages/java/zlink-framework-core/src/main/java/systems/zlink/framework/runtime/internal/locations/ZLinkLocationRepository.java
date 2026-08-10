package systems.zlink.framework.runtime.internal.locations;

import java.time.Duration;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

/**
 * Framework-private repository for decoded location and relocation state.
 */
public interface ZLinkLocationRepository {
    CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent);

    CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner);

    CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(
        String meshName,
        ZLinkPageRequest page);

    CompletionStage<ZLinkLocationWriteResult> updateClientServer(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent);

    CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner);

    CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
        listClientServers(String channelName, ZLinkPageRequest page);

    CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent);

    CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner);

    CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
        listFanoutPublishers(String channelName, ZLinkPageRequest page);

    CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
        String ownerId,
        Duration leaseTtl);

    CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId);

    CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
        ZLinkLocationOwnerToken token,
        Duration leaseTtl);

    CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
        ZLinkLocationOwnerToken token);

    CompletionStage<ZLinkAuthorityReadResult> read(
        String key,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
        String key,
        ZLinkAuthorityExpectation expectation,
        ZLinkAuthorityMutation mutation,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAuthorityScanResult> list(
        String prefix,
        Optional<ZLinkAuthorityScanCursor> cursor,
        int limit,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectReserveResult> reserve(
        ZLinkObjectReservationRequest request,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectRejectResult> reject(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(
        ZLinkCreationOperationIdentity operation,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkRelocationCapacityReserveResult>
        reserveRelocationCapacity(
            ZLinkRelocationCapacityReservationRequest request,
            ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkRelocationCapacityAbortResult>
        abortRelocationCapacity(
            ZLinkRelocationCapacityFence fence,
            ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
        ZLinkAggregatePrepareRequest request,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation);

    /**
     * Records an aborted authority decision while retaining the immutable
     * aggregate inventory for an external terminal-recovery operation.
     */
    default CompletionStage<ZLinkAggregateAbortRecoverySnapshot>
        retainAggregateAbort(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "retained aggregate abort is not supported by this repository"));
    }

    /** Lists abort tombstones whose external terminal is still pending. */
    default CompletionStage<List<ZLinkAggregateAbortRecoverySnapshot>>
        listRetainedAggregateAborts(ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "retained aggregate abort is not supported by this repository"));
    }

    /**
     * Records the proven external terminal and preserves its root identity for
     * crash-resumable cleanup.
     */
    default CompletionStage<Optional<ZLinkAggregateAbortCleanupSnapshot>>
        markAggregateAbortTerminal(
        ZLinkAggregateFence fence,
        String expectedStoreVersion,
        String reference,
        long checksumCrc32c,
        ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "aggregate abort terminal cleanup is not supported by this repository"));
    }

    /** Lists terminal cleanups that must resume before retained-abort recovery. */
    default CompletionStage<List<ZLinkAggregateAbortCleanupSnapshot>>
        listTerminalAggregateAborts(ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "aggregate abort terminal cleanup is not supported by this repository"));
    }

    /** Removes participant markers and inventory while retaining the tombstone. */
    default CompletionStage<Boolean> cleanupTerminalAggregateAbortInventory(
        ZLinkAggregateAbortCleanupSnapshot cleanup,
        ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "aggregate abort terminal cleanup is not supported by this repository"));
    }

    /** Removes the exact terminal cleanup tombstone as the final durable step. */
    default CompletionStage<Boolean> removeTerminalAggregateAbort(
        ZLinkAggregateAbortCleanupSnapshot cleanup,
        ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "aggregate abort terminal cleanup is not supported by this repository"));
    }

    /** Reads the private committed aggregate marker, if one exists. */
    default CompletionStage<Optional<ZLinkAggregateProgressSnapshot>>
        readAggregateProgress(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "aggregate progress is not supported by this repository"));
    }

    /** Performs the single StoreVersion CAS for a committed aggregate root. */
    default CompletionStage<ZLinkAggregateProgressWriteResult>
        compareExchangeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            ZLinkAggregateProgress progress,
            ZLinkStoreCancellation cancellation) {
        return CompletableFuture.failedFuture(
            new UnsupportedOperationException(
                "aggregate progress is not supported by this repository"));
    }

    /** Lists committed aggregate markers for restart reconciliation. */
    default CompletionStage<List<ZLinkAggregateProgressSnapshot>>
        listAggregateProgress(ZLinkStoreCancellation cancellation) {
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

    default CompletionStage<OptionalLong> getMeshNodeChangeStamp(
        String meshName) {
        return CompletableFuture.completedFuture(OptionalLong.empty());
    }
}
