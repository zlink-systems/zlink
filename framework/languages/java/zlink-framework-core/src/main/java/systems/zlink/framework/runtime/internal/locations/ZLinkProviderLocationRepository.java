package systems.zlink.framework.runtime.internal.locations;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;

import java.time.Duration;
import java.util.Objects;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

/**
 * Keeps the public opaque provider boundary separate from Framework-owned
 * location records.
 */
public final class ZLinkProviderLocationRepository
    implements ZLinkLocationRepository {
    private final ZLinkLocationStore
        provider;
    private final ZLinkProviderOwnerLeaseRepository owners;
    private final ZLinkProviderDescriptorRepository descriptors;
    private final ZLinkProviderAuthorityRepository authority;

    public ZLinkProviderLocationRepository(
        ZLinkLocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
        this.owners = new ZLinkProviderOwnerLeaseRepository(provider);
        this.descriptors = new ZLinkProviderDescriptorRepository(provider);
        this.authority = new ZLinkProviderAuthorityRepository(
            provider,
            this.descriptors);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        return descriptors.updateMeshNode(descriptor, intent);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return descriptors.removeMeshNode(key, owner);
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
        listMeshNodes(String meshName, ZLinkPageRequest page) {
        return descriptors.listMeshNodes(meshName, page);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateClientServer(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        return descriptors.updateClientServer(descriptor, intent);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return descriptors.removeClientServer(key, owner);
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
        listClientServers(String channelName, ZLinkPageRequest page) {
        return descriptors.listClientServers(channelName, page);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        return descriptors.updateFanoutPublisher(descriptor, intent);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return descriptors.removeFanoutPublisher(key, owner);
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
        listFanoutPublishers(String channelName, ZLinkPageRequest page) {
        return descriptors.listFanoutPublishers(channelName, page);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
        String ownerId,
        Duration leaseTtl) {
        return owners.claim(ownerId, leaseTtl);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(
        String ownerId) {
        return owners.read(ownerId);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
        ZLinkLocationOwnerToken token,
        Duration leaseTtl) {
        return owners.renew(token, leaseTtl);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
        ZLinkLocationOwnerToken token) {
        return owners.release(token);
    }

    @Override
    public CompletionStage<ZLinkAuthorityReadResult> read(
        String key,
        ZLinkStoreCancellation cancellation) {
        return authority().read(key, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
        String key,
        ZLinkAuthorityExpectation expectation,
        ZLinkAuthorityMutation mutation,
        ZLinkStoreCancellation cancellation) {
        return authority().compareExchange(
            key, expectation, mutation, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAuthorityScanResult> list(
        String prefix,
        Optional<ZLinkAuthorityScanCursor> cursor,
        int limit,
        ZLinkStoreCancellation cancellation) {
        return authority().list(prefix, cursor, limit, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectReserveResult> reserve(
        ZLinkObjectReservationRequest request,
        ZLinkStoreCancellation cancellation) {
        return authority().reserve(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkStoreCancellation cancellation) {
        return authority.commit(
            reservation, readyPayload, null, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        return authority.commit(
            reservation, readyPayload, terminal, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectRejectResult> reject(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        return authority.reject(reservation, terminal, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkStoreCancellation cancellation) {
        return authority.abort(reservation, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        return authority.abort(reservation, cancellation);
    }

    @Override
    public CompletionStage<ZLinkCreationTerminalReadResult>
        readCreationTerminal(
            ZLinkCreationOperationIdentity operation,
            ZLinkStoreCancellation cancellation) {
        return authority.readCreationTerminal(operation, cancellation);
    }

    @Override
    public CompletionStage<ZLinkRelocationCapacityReserveResult>
        reserveRelocationCapacity(
            ZLinkRelocationCapacityReservationRequest request,
            ZLinkStoreCancellation cancellation) {
        return authority.reserveRelocationCapacity(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkRelocationCapacityAbortResult>
        abortRelocationCapacity(
            ZLinkRelocationCapacityFence fence,
            ZLinkStoreCancellation cancellation) {
        return authority.abortRelocationCapacity(fence, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
        ZLinkAggregatePrepareRequest request,
        ZLinkStoreCancellation cancellation) {
        return authority.prepareAggregate(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        return authority.commitAggregate(fence, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        return authority.abortAggregate(fence, cancellation);
    }

    @Override
    public CompletionStage<Optional<ZLinkAggregateProgressSnapshot>>
        readAggregateProgress(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
        return authority.readAggregateProgress(fence, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAggregateProgressWriteResult>
        compareExchangeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            ZLinkAggregateProgress progress,
            ZLinkStoreCancellation cancellation) {
        return authority.compareExchangeAggregateProgress(
            fence, expectedStoreVersion, progress, cancellation);
    }

    @Override
    public CompletionStage<List<ZLinkAggregateProgressSnapshot>>
        listAggregateProgress(ZLinkStoreCancellation cancellation) {
        return authority.listAggregateProgress(cancellation);
    }

    @Override
    public CompletionStage<ZLinkAggregateAbortRecoverySnapshot>
        retainAggregateAbort(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
        return authority.retainAggregateAbort(fence, cancellation);
    }

    @Override
    public CompletionStage<List<ZLinkAggregateAbortRecoverySnapshot>>
        listRetainedAggregateAborts(
            ZLinkStoreCancellation cancellation) {
        return authority.listRetainedAggregateAborts(cancellation);
    }

    @Override
    public CompletionStage<Optional<ZLinkAggregateAbortCleanupSnapshot>>
        markAggregateAbortTerminal(
        ZLinkAggregateFence fence,
        String expectedStoreVersion,
        String reference,
        long checksumCrc32c,
        ZLinkStoreCancellation cancellation) {
        return authority.markAggregateAbortTerminal(
            fence,
            expectedStoreVersion,
            reference,
            checksumCrc32c,
            cancellation);
    }

    @Override
    public CompletionStage<List<ZLinkAggregateAbortCleanupSnapshot>>
        listTerminalAggregateAborts(
            ZLinkStoreCancellation cancellation) {
        return authority.listTerminalAggregateAborts(cancellation);
    }

    @Override
    public CompletionStage<Boolean> cleanupTerminalAggregateAbortInventory(
        ZLinkAggregateAbortCleanupSnapshot cleanup,
        ZLinkStoreCancellation cancellation) {
        return authority.cleanupTerminalAggregateAbortInventory(
            cleanup, cancellation);
    }

    @Override
    public CompletionStage<Boolean> removeTerminalAggregateAbort(
        ZLinkAggregateAbortCleanupSnapshot cleanup,
        ZLinkStoreCancellation cancellation) {
        return authority.removeTerminalAggregateAbort(cleanup, cancellation);
    }

    @Override
    public CompletionStage<Boolean> removeAggregateProgress(
        ZLinkAggregateFence fence,
        String expectedStoreVersion,
        ZLinkStoreCancellation cancellation) {
        return authority.removeAggregateProgress(
            fence, expectedStoreVersion, cancellation);
    }

    @Override
    public CompletionStage<Long> removeAllByOwner(
        ZLinkLocationOwnerToken owner) {
        return authority.removeAllByOwner(owner);
    }

    @Override
    public CompletionStage<OptionalLong> getMeshNodeChangeStamp(
        String meshName) {
        return CompletableFuture.completedFuture(
            OptionalLong.empty());
    }

    public ZLinkLocationStore
        provider() {
        return provider;
    }

    private ZLinkProviderAuthorityRepository authority() {
        return authority;
    }
}
