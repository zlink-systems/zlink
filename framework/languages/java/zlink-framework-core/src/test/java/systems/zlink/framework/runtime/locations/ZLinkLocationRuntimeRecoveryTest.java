package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewStale;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

final class ZLinkLocationRuntimeRecoveryTest {
    @Test
    void staleOwnerLeaseRenewalReclaimsANewGeneration() {
        StaleThenClaimStore store = new StaleThenClaimStore();
        try (ZLinkLocationRuntime runtime = new ZLinkLocationRuntime(
            store,
            "owner-a",
            Duration.ofSeconds(30),
            Duration.ofSeconds(5))) {
            runtime.start(RoutingId.from("node-a"))
                .toCompletableFuture()
                .join();

            assertTrue(runtime.renewOwnerLeaseOnce()
                .toCompletableFuture()
                .join());
            assertTrue(runtime.ownerLeaseHealthy());
            assertEquals(2, store.claimCount.get());
            assertEquals(2, runtime.currentOwnerToken().leaseGeneration());
        }
    }

    private static final class StaleThenClaimStore
        extends ZLinkLocationStoreTestAdapter {
        private final AtomicInteger claimCount = new AtomicInteger();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
            String ownerId,
            Duration ttl) {
            int generation = claimCount.incrementAndGet();
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseClaimed(
                new ZLinkLocationOwnerToken(ownerId, generation),
                now.plus(ttl),
                now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
            ZLinkLocationOwnerToken token,
            Duration ttl) {
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
            ZLinkLocationOwnerToken token) {
            return CompletableFuture.completedFuture(ZLinkOwnerLeaseReleaseResult.RELEASED);
        }
    }
}
