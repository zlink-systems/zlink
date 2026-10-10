package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;

import java.time.Duration;
import java.util.Objects;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;

/** Keeps the public opaque provider boundary separate from Framework-owned location records. */
public final class ZLinkProviderLocationRepository implements ZLinkLocationRepository {
    private final ProviderBoundary provider;
    private final ZLinkProviderOwnerLeaseRepository owners;
    private final ZLinkProviderDescriptorRepository descriptors;
    private final ZLinkProviderAuthorityRepository authority;

    public ZLinkProviderLocationRepository(ZLinkLocationStore provider) {
        this.provider = new ProviderBoundary(Objects.requireNonNull(provider, "provider"));
        this.owners = new ZLinkProviderOwnerLeaseRepository(this.provider);
        this.descriptors = new ZLinkProviderDescriptorRepository(this.provider);
        this.authority = new ZLinkProviderAuthorityRepository(this.provider, this.descriptors);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
            ZLinkMeshNodeDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        return descriptors.updateMeshNode(descriptor, intent);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
            ZLinkMeshNodeDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return descriptors.removeMeshNode(key, owner);
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(
            String meshName, ZLinkPageRequest page) {
        return descriptors.listMeshNodes(meshName, page);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateClientServer(
            ZLinkClientServerServerDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        return descriptors.updateClientServer(descriptor, intent);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
            ZLinkClientServerServerDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return descriptors.removeClientServer(key, owner);
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> listClientServers(
            String channelName, ZLinkPageRequest page) {
        return descriptors.listClientServers(channelName, page);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
            ZLinkFanoutPublisherDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        return descriptors.updateFanoutPublisher(descriptor, intent);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
            ZLinkFanoutPublisherDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return descriptors.removeFanoutPublisher(key, owner);
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> listFanoutPublishers(
            String channelName, ZLinkPageRequest page) {
        return descriptors.listFanoutPublishers(channelName, page);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
            String ownerId, Duration leaseTtl) {
        return owners.claim(ownerId, leaseTtl);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
        return owners.read(ownerId);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
            ZLinkLocationOwnerToken token, Duration leaseTtl) {
        return owners.renew(token, leaseTtl);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
            ZLinkLocationOwnerToken token) {
        return owners.release(token);
    }

    @Override
    public CompletionStage<ZLinkAuthorityReadResult> read(
            String key, ZLinkStoreCancellation cancellation) {
        return authority().read(key, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key,
            ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation,
            ZLinkStoreCancellation cancellation) {
        return authority().compareExchange(key, expectation, mutation, cancellation);
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
    public CompletionStage<Boolean> releaseEndedReservation(
            String key, String expectedStoreVersion, ZLinkStoreCancellation cancellation) {
        return authority()
                .releaseEndedReservation(
                        key,
                        expectedStoreVersion,
                        cancellation == null ? null : cancellation::isCancellationRequested);
    }

    @Override
    public CompletionStage<Boolean> releaseEndedReservation(
            ZLinkObjectReservationRequest request,
            String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
        return authority.releaseEndedReservation(
                request, expectedStoreVersion, cancellation::isCancellationRequested);
    }

    @Override
    public CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request, ZLinkStoreCancellation cancellation) {
        return authority().reserve(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkStoreCancellation cancellation) {
        return authority.commit(reservation, readyPayload, null, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        return authority.commit(reservation, readyPayload, terminal, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation, byte[] readyPayload, long deadlineUnixMs) {
        return authority.commit(reservation, readyPayload, deadlineUnixMs);
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            long deadlineUnixMs) {
        return authority.commit(reservation, readyPayload, terminal, deadlineUnixMs);
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
            ZLinkObjectReservation reservation, ZLinkStoreCancellation cancellation) {
        return authority.abort(reservation, cancellation);
    }

    @Override
    public CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        return authority.abort(reservation, terminal, cancellation);
    }

    @Override
    public CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(
            ZLinkCreationOperationIdentity operation, ZLinkStoreCancellation cancellation) {
        return authority.readCreationTerminal(operation, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation) {
        return authority.prepareAggregate(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        return authority.commitAggregate(fence, cancellation);
    }

    @Override
    public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        return authority.abortAggregate(fence, cancellation);
    }

    @Override
    public CompletionStage<Optional<ZLinkAggregateProgressSnapshot>> readAggregateProgress(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        return authority.readAggregateProgress(fence, cancellation);
    }

    @Override
    public CompletionStage<Boolean> removeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
        return authority.removeAggregateProgress(fence, expectedStoreVersion, cancellation);
    }

    @Override
    public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken owner) {
        return authority.removeAllByOwner(owner);
    }

    @Override
    public CompletionStage<OptionalLong> getMeshNodeChangeStamp(String meshName) {
        return CompletableFuture.completedFuture(OptionalLong.empty());
    }

    public ZLinkLocationStore provider() {
        return provider.delegate();
    }

    private ZLinkProviderAuthorityRepository authority() {
        return authority;
    }

    /** Classifies only SPI failures; record decoding remains outside this boundary. */
    private record ProviderBoundary(ZLinkLocationStore delegate) implements ZLinkLocationStore {
        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            try {
                return observe(delegate.read(key, cancellation), cancellation);
            } catch (RuntimeException error) {
                throw failure(error, cancellation);
            }
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            try {
                return observe(delegate.write(request, cancellation), cancellation);
            } catch (RuntimeException error) {
                throw failure(error, cancellation);
            }
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            try {
                return observe(delegate.scan(request, cancellation), cancellation);
            } catch (RuntimeException error) {
                throw failure(error, cancellation);
            }
        }

        private static <T> CompletionStage<T> observe(
                CompletionStage<T> operation,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return operation.handle(
                    (value, error) -> {
                        if (error != null) throw failure(error, cancellation);
                        return value;
                    });
        }

        private static RuntimeException failure(
                Throwable error,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            while ((error instanceof CompletionException || error instanceof ExecutionException)
                    && error.getCause() != null) error = error.getCause();
            if (error instanceof ZLinkFrameworkException
                    || error instanceof IllegalArgumentException
                    || (cancellation != null && cancellation.isCancellationRequested())) {
                return error instanceof RuntimeException runtime
                        ? runtime
                        : new CompletionException(error);
            }
            if (error instanceof Error fatal) throw fatal;
            return new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "Location Store provider is unavailable.",
                    error);
        }
    }
}
