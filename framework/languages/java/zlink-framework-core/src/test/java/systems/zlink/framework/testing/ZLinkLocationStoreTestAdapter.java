package systems.zlink.framework.testing;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;

/** Test-only adapter that lets focused tests override only the store operations they exercise. */
public abstract class ZLinkLocationStoreTestAdapter implements ZLinkLocationRepository {
    private static <T> CompletionStage<T> unsupported() {
        return CompletableFuture.failedFuture(new UnsupportedOperationException());
    }

    @Override public CompletionStage<ZLinkLocationWriteResult> updateMeshNode(ZLinkMeshNodeDescriptor value, ZLinkLocationWriteIntent intent) { return unsupported(); }
    @Override public CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(ZLinkMeshNodeDescriptorKey key, ZLinkLocationOwnerToken owner) { return unsupported(); }
    @Override public CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(String meshName, ZLinkPageRequest page) { return unsupported(); }
    @Override public CompletionStage<ZLinkLocationWriteResult> updateClientServer(ZLinkClientServerServerDescriptor value, ZLinkLocationWriteIntent intent) { return unsupported(); }
    @Override public CompletionStage<ZLinkLocationWriteStatus> removeClientServer(ZLinkClientServerServerDescriptorKey key, ZLinkLocationOwnerToken owner) { return unsupported(); }
    @Override public CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> listClientServers(String channelName, ZLinkPageRequest page) { return unsupported(); }
    @Override public CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(ZLinkFanoutPublisherDescriptor value, ZLinkLocationWriteIntent intent) { return unsupported(); }
    @Override public CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(ZLinkFanoutPublisherDescriptorKey key, ZLinkLocationOwnerToken owner) { return unsupported(); }
    @Override public CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> listFanoutPublishers(String channelName, ZLinkPageRequest page) { return unsupported(); }
    @Override public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(String ownerId, Duration ttl) { return unsupported(); }
    @Override public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) { return unsupported(); }
    @Override public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(ZLinkLocationOwnerToken token, Duration ttl) { return unsupported(); }
    @Override public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(ZLinkLocationOwnerToken token) { return unsupported(); }
    @Override public CompletionStage<ZLinkAuthorityReadResult> read(String key, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(String key, ZLinkAuthorityExpectation expectation, ZLinkAuthorityMutation mutation, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkAuthorityScanResult> list(String prefix, Optional<ZLinkAuthorityScanCursor> cursor, int limit, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkObjectReserveResult> reserve(ZLinkObjectReservationRequest request, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkObjectCommitResult> commit(ZLinkObjectReservation reservation, byte[] payload, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkObjectCommitResult> commit(ZLinkObjectReservation reservation, byte[] payload, ZLinkCreationOperationTerminal terminal, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkObjectRejectResult> reject(ZLinkObjectReservation reservation, ZLinkCreationOperationTerminal terminal, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkObjectAbortResult> abort(ZLinkObjectReservation reservation, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkObjectAbortResult> abort(ZLinkObjectReservation reservation, ZLinkCreationOperationTerminal terminal, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(ZLinkCreationOperationIdentity operation, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkRelocationCapacityReserveResult> reserveRelocationCapacity(ZLinkRelocationCapacityReservationRequest request, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkRelocationCapacityAbortResult> abortRelocationCapacity(ZLinkRelocationCapacityFence fence, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) { return unsupported(); }
    @Override public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken owner) { return unsupported(); }
}
