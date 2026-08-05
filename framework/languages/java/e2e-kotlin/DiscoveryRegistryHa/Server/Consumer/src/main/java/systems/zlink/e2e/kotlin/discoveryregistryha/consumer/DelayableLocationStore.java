package systems.zlink.e2e.kotlin.discoveryregistryha.consumer;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

public final class DelayableLocationStore implements ZLinkLocationStore {
    private final ZLinkLocationStore inner;
    private final LocationStoreDelayState delayState;

    public DelayableLocationStore(ZLinkLocationStore inner, LocationStoreDelayState delayState) {
        this.inner = inner;
        this.delayState = delayState;
    }

    private <T> CompletionStage<T> delayed(Supplier<CompletionStage<T>> action) {
        int delay = delayState.delayMilliseconds();
        if (delay <= 0) return action.get();
        return CompletableFuture.runAsync(
                () -> { },
                CompletableFuture.delayedExecutor(delay, TimeUnit.MILLISECONDS))
            .thenCompose(ignored -> action.get());
    }

    @Override public CompletionStage<ZLinkLocationWriteResult> updateMeshNode(ZLinkMeshNodeDescriptor value, ZLinkLocationWriteIntent intent) { return delayed(() -> inner.updateMeshNode(value, intent)); }
    @Override public CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(ZLinkMeshNodeDescriptorKey key, ZLinkLocationOwnerToken owner) { return delayed(() -> inner.removeMeshNode(key, owner)); }
    @Override public CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(String meshName, ZLinkPageRequest page) { return delayed(() -> inner.listMeshNodes(meshName, page)); }
    @Override public CompletionStage<ZLinkLocationWriteResult> updateClientServer(ZLinkClientServerServerDescriptor value, ZLinkLocationWriteIntent intent) { return delayed(() -> inner.updateClientServer(value, intent)); }
    @Override public CompletionStage<ZLinkLocationWriteStatus> removeClientServer(ZLinkClientServerServerDescriptorKey key, ZLinkLocationOwnerToken owner) { return delayed(() -> inner.removeClientServer(key, owner)); }
    @Override public CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> listClientServers(String channelName, ZLinkPageRequest page) { return delayed(() -> inner.listClientServers(channelName, page)); }
    @Override public CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(ZLinkFanoutPublisherDescriptor value, ZLinkLocationWriteIntent intent) { return delayed(() -> inner.updateFanoutPublisher(value, intent)); }
    @Override public CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(ZLinkFanoutPublisherDescriptorKey key, ZLinkLocationOwnerToken owner) { return delayed(() -> inner.removeFanoutPublisher(key, owner)); }
    @Override public CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> listFanoutPublishers(String channelName, ZLinkPageRequest page) { return delayed(() -> inner.listFanoutPublishers(channelName, page)); }
    @Override public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(String ownerId, Duration leaseTtl) { return delayed(() -> inner.claimOwnerLease(ownerId, leaseTtl)); }
    @Override public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) { return delayed(() -> inner.readOwnerLease(ownerId)); }
    @Override public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(ZLinkLocationOwnerToken token, Duration leaseTtl) { return delayed(() -> inner.renewOwnerLease(token, leaseTtl)); }
    @Override public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(ZLinkLocationOwnerToken token) { return delayed(() -> inner.releaseOwnerLease(token)); }
    @Override public CompletionStage<ZLinkAuthorityReadResult> read(String key, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.read(key, cancellation)); }
    @Override public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(String key, ZLinkAuthorityExpectation expectation, ZLinkAuthorityMutation mutation, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.compareExchange(key, expectation, mutation, cancellation)); }
    @Override public CompletionStage<ZLinkAuthorityScanResult> list(String prefix, Optional<ZLinkAuthorityScanCursor> cursor, int limit, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.list(prefix, cursor, limit, cancellation)); }
    @Override public CompletionStage<ZLinkObjectReserveResult> reserve(ZLinkObjectReservationRequest request, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.reserve(request, cancellation)); }
    @Override public CompletionStage<ZLinkObjectCommitResult> commit(ZLinkObjectReservation reservation, byte[] payload, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.commit(reservation, payload, cancellation)); }
    @Override public CompletionStage<ZLinkObjectCommitResult> commit(ZLinkObjectReservation reservation, byte[] payload, ZLinkCreationOperationTerminal terminal, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.commit(reservation, payload, terminal, cancellation)); }
    @Override public CompletionStage<ZLinkObjectRejectResult> reject(ZLinkObjectReservation reservation, ZLinkCreationOperationTerminal terminal, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.reject(reservation, terminal, cancellation)); }
    @Override public CompletionStage<ZLinkObjectAbortResult> abort(ZLinkObjectReservation reservation, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.abort(reservation, cancellation)); }
    @Override public CompletionStage<ZLinkObjectAbortResult> abort(ZLinkObjectReservation reservation, ZLinkCreationOperationTerminal terminal, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.abort(reservation, terminal, cancellation)); }
    @Override public CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(ZLinkCreationOperationIdentity operation, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.readCreationTerminal(operation, cancellation)); }
    @Override public CompletionStage<ZLinkRelocationCapacityReserveResult> reserveRelocationCapacity(ZLinkRelocationCapacityReservationRequest request, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.reserveRelocationCapacity(request, cancellation)); }
    @Override public CompletionStage<ZLinkRelocationCapacityAbortResult> abortRelocationCapacity(ZLinkRelocationCapacityFence fence, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.abortRelocationCapacity(fence, cancellation)); }
    @Override public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.prepareAggregate(request, cancellation)); }
    @Override public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.commitAggregate(fence, cancellation)); }
    @Override public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.abortAggregate(fence, cancellation)); }
    @Override public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken owner) { return delayed(() -> inner.removeAllByOwner(owner)); }
}
