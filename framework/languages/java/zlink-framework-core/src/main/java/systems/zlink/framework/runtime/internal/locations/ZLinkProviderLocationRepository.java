package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.LocationOperation;

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

    public AutoCloseable registerCapacityObservation() {
        return authority.registerCapacityObservation();
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
            ZLinkMeshNodeDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () ->
                        descriptors
                                .updateMeshNode(descriptor, intent)
                                .thenApply(
                                        result -> {
                                            if (result.status() == ZLinkLocationWriteStatus.STORED)
                                                authority.registerCapacityMetrics(descriptor);
                                            return result;
                                        }));
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
            ZLinkMeshNodeDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return observeStore(
                LocationOperation.RELEASE,
                () ->
                        descriptors
                                .removeMeshNode(key, owner)
                                .thenApply(
                                        result -> {
                                            if (result == ZLinkLocationWriteStatus.STORED)
                                                authority.removeCapacityMetrics(key);
                                            return result;
                                        }));
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(
            String meshName, ZLinkPageRequest page) {
        return observeStore(
                LocationOperation.READ,
                () ->
                        descriptors
                                .listMeshNodes(meshName, page)
                                .thenCompose(authority::refreshCapacityMetrics));
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateClientServer(
            ZLinkClientServerServerDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> descriptors.updateClientServer(descriptor, intent));
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
            ZLinkClientServerServerDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return observeStore(
                LocationOperation.RELEASE, () -> descriptors.removeClientServer(key, owner));
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> listClientServers(
            String channelName, ZLinkPageRequest page) {
        return observeStore(
                LocationOperation.READ, () -> descriptors.listClientServers(channelName, page));
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
            ZLinkFanoutPublisherDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> descriptors.updateFanoutPublisher(descriptor, intent));
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
            ZLinkFanoutPublisherDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return observeStore(
                LocationOperation.RELEASE, () -> descriptors.removeFanoutPublisher(key, owner));
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> listFanoutPublishers(
            String channelName, ZLinkPageRequest page) {
        return observeStore(
                LocationOperation.READ, () -> descriptors.listFanoutPublishers(channelName, page));
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
            String ownerId, Duration leaseTtl) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE, () -> owners.claim(ownerId, leaseTtl));
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
        return owners.read(ownerId);
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
            ZLinkLocationOwnerToken token, Duration leaseTtl) {
        return observeStore(LocationOperation.READ, () -> owners.renew(token, leaseTtl));
    }

    @Override
    public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
            ZLinkLocationOwnerToken token) {
        return observeStore(LocationOperation.RELEASE, () -> owners.release(token));
    }

    @Override
    public CompletionStage<ZLinkAuthorityReadResult> read(
            String key, ZLinkStoreCancellation cancellation) {
        return observeStore(LocationOperation.READ, () -> authority().read(key, cancellation));
    }

    @Override
    public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key,
            ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation,
            ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority().compareExchange(key, expectation, mutation, cancellation));
    }

    @Override
    public CompletionStage<ZLinkAuthorityScanResult> list(
            String prefix,
            Optional<ZLinkAuthorityScanCursor> cursor,
            int limit,
            ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.READ,
                () -> authority().list(prefix, cursor, limit, cancellation));
    }

    @Override
    public CompletionStage<Boolean> releaseEndedReservation(
            String key, String expectedStoreVersion, ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.RELEASE,
                () ->
                        authority()
                                .releaseEndedReservation(
                                        key,
                                        expectedStoreVersion,
                                        cancellation == null
                                                ? null
                                                : cancellation::isCancellationRequested));
    }

    @Override
    public CompletionStage<Boolean> releaseEndedReservation(
            ZLinkObjectReservationRequest request,
            String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.RELEASE,
                () ->
                        authority.releaseEndedReservation(
                                request,
                                expectedStoreVersion,
                                cancellation::isCancellationRequested));
    }

    @Override
    public CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request, ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority().reserve(request, cancellation));
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.commit(reservation, readyPayload, null, cancellation));
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.commit(reservation, readyPayload, terminal, cancellation));
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation, byte[] readyPayload, long deadlineUnixMs) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.commit(reservation, readyPayload, deadlineUnixMs));
    }

    @Override
    public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            long deadlineUnixMs) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.commit(reservation, readyPayload, terminal, deadlineUnixMs));
    }

    @Override
    public CompletionStage<ZLinkObjectRejectResult> reject(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.reject(reservation, terminal, cancellation));
    }

    @Override
    public CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation, ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.abort(reservation, cancellation));
    }

    @Override
    public CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.abort(reservation, terminal, cancellation));
    }

    @Override
    public CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(
            ZLinkCreationOperationIdentity operation, ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.READ,
                () -> authority.readCreationTerminal(operation, cancellation));
    }

    @Override
    public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.prepareAggregate(request, cancellation));
    }

    @Override
    public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.commitAggregate(fence, cancellation));
    }

    @Override
    public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.COMPARE_EXCHANGE,
                () -> authority.abortAggregate(fence, cancellation));
    }

    @Override
    public CompletionStage<Optional<ZLinkAggregateProgressSnapshot>> readAggregateProgress(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.READ, () -> authority.readAggregateProgress(fence, cancellation));
    }

    @Override
    public CompletionStage<Boolean> removeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
        return observeStore(
                LocationOperation.RELEASE,
                () -> authority.removeAggregateProgress(fence, expectedStoreVersion, cancellation));
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

    private static <T> CompletionStage<T> observeStore(
            LocationOperation operation, java.util.function.Supplier<CompletionStage<T>> call) {
        CompletionStage<T> result;
        try {
            result = call.get();
        } catch (RuntimeException failure) {
            recordStoreFailure(operation, failure);
            throw failure;
        }
        return result.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) recordStoreFailure(operation, failure);
                });
    }

    private static void recordStoreFailure(LocationOperation operation, Throwable failure) {
        while ((failure instanceof CompletionException || failure instanceof ExecutionException)
                && failure.getCause() != null) failure = failure.getCause();
        if (failure instanceof ZLinkLocationStoreFailure) {
            operation.failure();
        }
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
            if ((error instanceof ZLinkFrameworkException typed
                            && typed.kind() != ZLinkFrameworkErrorKind.UNAVAILABLE)
                    || error instanceof IllegalArgumentException
                    || (cancellation != null && cancellation.isCancellationRequested())) {
                return error instanceof RuntimeException runtime
                        ? runtime
                        : new CompletionException(error);
            }
            if (error instanceof Error fatal) throw fatal;
            return new ZLinkLocationStoreFailureException(error);
        }
    }
}
