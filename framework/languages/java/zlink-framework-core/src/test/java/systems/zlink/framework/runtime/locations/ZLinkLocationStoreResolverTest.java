package systems.zlink.framework.runtime.locations;
import java.time.Clock;
import java.util.Optional;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMutation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanCursor;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationIdentity;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectRejectResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserveResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewal;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseSnapshot;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkProviderLocationRepository;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

class ZLinkLocationStoreResolverTest {
    @Test
    void disabledRegistrationResolvesToNull() {
        assertNull(ZLinkLocationStoreResolver.resolve(
            new ZLinkLocationRegistration(),
            ZLinkHandlerActivator.reflection()));
    }

    @Test
    void inMemoryRegistrationUsesOneStoreForAllRolesAndOptionalStampStore() {
        ZLinkLocationRegistration registration = new ZLinkLocationRegistration();
        registration.setStoreInstance(new ZLinkInMemoryLocationStore());

        ZLinkRegisteredLocationStores stores = ZLinkLocationStoreResolver.resolve(
            registration,
            ZLinkHandlerActivator.reflection());

        assertNotNull(stores);
        assertSame(stores.peerStore(), stores.spotStore());
        assertSame(stores.peerStore(), stores.actorStore());
        assertSame(stores.peerStore(), stores.routeStore());
        assertSame(stores.peerStore(), stores.ownerLeaseStore());
        assertSame(stores.peerStore(), stores.unifiedStore());
    }

    @Test
    void explicitUnifiedStoreInstanceIsReusedForAllRoles() {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore();
        ZLinkLocationRegistration registration = new ZLinkLocationRegistration();
        registration.setStoreInstance(store);

        ZLinkRegisteredLocationStores stores = ZLinkLocationStoreResolver.resolve(
            registration,
            ZLinkHandlerActivator.reflection());

        assertSame(stores.peerStore(), stores.spotStore());
        assertSame(stores.peerStore(), stores.actorStore());
        assertSame(stores.peerStore(), stores.routeStore());
        assertSame(stores.peerStore(), stores.ownerLeaseStore());
        assertSame(stores.peerStore(), stores.unifiedStore());
        assertSame(
            store,
            ((ZLinkProviderLocationRepository) stores.unifiedStore())
                .provider());
    }

    @Test
    void explicitUnifiedStoreTypeIsSharedThroughEveryRole() {
        CountingLocationStore.created.set(0);
        CountingLocationStore store = new CountingLocationStore();
        ZLinkLocationRegistration registration = new ZLinkLocationRegistration();
        registration.setStoreInstance(store);

        ZLinkRegisteredLocationStores stores = ZLinkLocationStoreResolver.resolve(
            registration,
            ZLinkHandlerActivator.reflection());

        assertEquals(1, CountingLocationStore.created.get());
        assertSame(
            store,
            ((ZLinkProviderLocationRepository) stores.unifiedStore())
                .provider());
        assertSame(stores.peerStore(), stores.spotStore());
        assertSame(stores.peerStore(), stores.actorStore());
        assertSame(stores.peerStore(), stores.routeStore());
        assertSame(stores.peerStore(), stores.ownerLeaseStore());
        assertSame(stores.peerStore(), stores.unifiedStore());
    }

    @Test
    void ownerLeaseRefreshTimeDoesNotMoveBackwardWhenStoreClockRegresses() {
        DescendingLeaseClockStore store = new DescendingLeaseClockStore(
            Instant.parse("2026-07-13T14:04:23.422Z"),
            Instant.parse("2026-07-13T14:04:23.231Z"));
        try (ZLinkLocationRuntime runtime = new ZLinkLocationRuntime(
                store,
                "owner-a",
                Duration.ofMinutes(1),
                Duration.ofDays(1))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            Instant before = runtime.ownerLeaseRenewedAt();

            runtime.renewOwnerLeaseOnce().toCompletableFuture().join();

            assertTrue(runtime.ownerLeaseRenewedAt().isAfter(before));
        }
    }

    public static class CountingLocationStore
        extends ZLinkLocationStoreTestAdapter
        implements systems.zlink.framework.locationprovider
            .ZLinkLocationStore {
        static final AtomicInteger created = new AtomicInteger();
        private final ZLinkInMemoryProviderLocationStore opaque =
            new ZLinkInMemoryProviderLocationStore();
        private final ZLinkInMemoryAuthorityStore authority =
            new ZLinkInMemoryAuthorityStore(
                Clock.systemUTC(),
                ignored -> true);

        public CountingLocationStore() {
            created.incrementAndGet();
        }

        @Override
        public CompletionStage<systems.zlink.framework.locationprovider
            .ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider
                    .ZLinkStoreCancellation cancellation) {
            return opaque.read(key, cancellation);
        }

        @Override
        public CompletionStage<systems.zlink.framework.locationprovider
            .ZLinkStoreWriteResult> write(
                systems.zlink.framework.locationprovider
                    .ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider
                    .ZLinkStoreCancellation cancellation) {
            return opaque.write(request, cancellation);
        }

        @Override
        public CompletionStage<systems.zlink.framework.locationprovider
            .ZLinkStoreScanResult> scan(
                systems.zlink.framework.locationprovider
                    .ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider
                    .ZLinkStoreCancellation cancellation) {
            return opaque.scan(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkAuthorityReadResult> read(
            String key,
            ZLinkStoreCancellation cancellation) {
            return authority.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkAuthorityWriteResult>
            compareExchange(
                String key,
                ZLinkAuthorityExpectation expectation,
                ZLinkAuthorityMutation mutation,
                ZLinkStoreCancellation cancellation) {
            return authority.compareExchange(key, expectation, mutation, cancellation);
        }

        @Override
        public CompletionStage<ZLinkAuthorityScanResult> list(
            String prefix,
            Optional<ZLinkAuthorityScanCursor> cursor,
            int limit,
            ZLinkStoreCancellation cancellation) {
            return authority.list(prefix, cursor, limit, cancellation);
        }

        @Override
        public CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request,
            ZLinkStoreCancellation cancellation) {
            return authority.reserve(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkStoreCancellation cancellation) {
            return authority.commit(reservation, readyPayload, cancellation);
        }

        @Override
        public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            return authority.commit(
                reservation,
                readyPayload,
                terminal,
                cancellation);
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
            return authority.abort(reservation, terminal, cancellation);
        }

        @Override
        public CompletionStage<ZLinkCreationTerminalReadResult>
            readCreationTerminal(
                ZLinkCreationOperationIdentity operation,
                ZLinkStoreCancellation cancellation) {
            return authority.readCreationTerminal(operation, cancellation);
        }

        @Override
        public CompletionStage<ZLinkAggregatePrepareResult>
            prepareAggregate(
                ZLinkAggregatePrepareRequest request,
                ZLinkStoreCancellation cancellation) {
            return authority.prepareAggregate(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkAggregateCommitResult>
            commitAggregate(
                ZLinkAggregateFence fence,
                ZLinkStoreCancellation cancellation) {
            return authority.commitAggregate(fence, cancellation);
        }

        @Override
        public CompletionStage<ZLinkAggregateAbortResult>
            abortAggregate(
                ZLinkAggregateFence fence,
                ZLinkStoreCancellation cancellation) {
            return authority.abortAggregate(fence, cancellation);
        }

        @Override
        public CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
            ZLinkMeshNodeDescriptor
                descriptor,
            ZLinkLocationWriteIntent intent) {
            return unsupportedWrite();
        }

        @Override
        public CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
                ZLinkMeshNodeDescriptorKey key,
                ZLinkLocationOwnerToken owner) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteStatus.IGNORED_STALE);
        }

        @Override
        public CompletionStage<ZLinkLocationPage<
            ZLinkMeshNodeDescriptor>> listMeshNodes(
                    String meshName,
                    ZLinkPageRequest page) {
            return CompletableFuture.completedFuture(
                new ZLinkLocationPage<>(List.of(), null));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult>
            claimOwnerLease(
            String ownerId,
            Duration leaseTtl) {
            return CompletableFuture.completedFuture(
                new ZLinkOwnerLeaseClaimed(
                    new ZLinkLocationOwnerToken(ownerId, 1L),
                    Instant.EPOCH.plus(leaseTtl),
                    Instant.EPOCH));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult>
            readOwnerLease(String ownerId) {
            return CompletableFuture.completedFuture(
                new ZLinkOwnerLeaseMissing());
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult>
            renewOwnerLease(
                ZLinkLocationOwnerToken token,
                Duration leaseTtl) {
            return CompletableFuture.completedFuture(
                new ZLinkOwnerLeaseRenewed(
                    Instant.EPOCH.plus(leaseTtl),
                    Instant.EPOCH));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult>
            releaseOwnerLease(ZLinkLocationOwnerToken token) {
            return CompletableFuture.completedFuture(
                ZLinkOwnerLeaseReleaseResult.RELEASED);
        }

        @Override
        public CompletionStage<Long> removeAllByOwner(
            ZLinkLocationOwnerToken owner) {
            return CompletableFuture.completedFuture(0L);
        }

        private CompletionStage<ZLinkLocationWriteResult> unsupportedWrite() {
            return CompletableFuture.failedFuture(new UnsupportedOperationException("write not supported"));
        }
    }

    private static final class DescendingLeaseClockStore extends CountingLocationStore {
        private final ArrayDeque<Instant> storeTimes;

        private DescendingLeaseClockStore(Instant... storeTimes) {
            this.storeTimes = new ArrayDeque<>(List.of(storeTimes));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult>
            claimOwnerLease(
            String ownerId,
            Duration leaseTtl) {
            Instant storeNow = storeTimes.removeFirst();
            return CompletableFuture.completedFuture(
                new ZLinkOwnerLeaseClaimed(
                    new ZLinkLocationOwnerToken(ownerId, 1L),
                    storeNow.plus(leaseTtl),
                    storeNow));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult>
            renewOwnerLease(
                ZLinkLocationOwnerToken token,
                Duration leaseTtl) {
            Instant storeNow = storeTimes.removeFirst();
            return CompletableFuture.completedFuture(
                new ZLinkOwnerLeaseRenewed(
                    storeNow.plus(leaseTtl),
                    storeNow));
        }
    }
}
