package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;

/** #4902: a renewal of the same token between release's read and delete must not keep the lease. */
final class ZLinkOwnerLeaseReleaseRaceTest {
    private static final Duration TTL = Duration.ofSeconds(30);

    @Test
    void releaseRetriesAfterARenewalOfTheSameTokenLandsFirst() {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var leases = new ZLinkProviderLocationRepository(inner);
        var claimed =
                assertInstanceOf(
                        ZLinkOwnerLeaseClaimed.class,
                        leases.claimOwnerLease("owner-renewed", TTL).toCompletableFuture().join());
        var renewed = new AtomicBoolean();
        ZLinkLocationStore provider =
                new ZLinkLocationStore() {
                    @Override
                    public CompletionStage<ZLinkStoreReadResult> read(
                            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
                        return inner.read(key, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkStoreWriteResult> write(
                            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
                        if (request.mutations().stream()
                                        .anyMatch(mutation -> mutation instanceof ZLinkStoreDelete)
                                && renewed.compareAndSet(false, true)) {
                            assertInstanceOf(
                                    ZLinkOwnerLeaseRenewed.class,
                                    leases.renewOwnerLease(claimed.token(), TTL)
                                            .toCompletableFuture()
                                            .join());
                        }
                        return inner.write(request, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkStoreScanResult> scan(
                            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
                        return inner.scan(request, cancellation);
                    }
                };
        var repository = new ZLinkProviderLocationRepository(provider);

        assertEquals(
                ZLinkOwnerLeaseReleaseResult.RELEASED,
                repository.releaseOwnerLease(claimed.token()).toCompletableFuture().join());
        assertTrue(renewed.get());
        assertInstanceOf(
                ZLinkOwnerLeaseMissing.class,
                repository.readOwnerLease("owner-renewed").toCompletableFuture().join());
        // A token that no longer holds the lease is still reported as stale.
        assertEquals(
                ZLinkOwnerLeaseReleaseResult.STALE,
                repository.releaseOwnerLease(claimed.token()).toCompletableFuture().join());
    }
}
