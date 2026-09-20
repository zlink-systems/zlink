package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewStale;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

final class ZLinkLocationRuntimeRecoveryTest {
    @Test
    void unavailableInitialClaimStartsWithoutAnOwnerAndHeartbeatClaimsAfterRecovery()
        throws Exception {
        ToggleClaimStore store = new ToggleClaimStore();
        CountDownLatch republished = new CountDownLatch(1);
        try (ZLinkLocationRuntime runtime = new ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores.fromUnified(store),
            "owner-a",
            Duration.ofSeconds(1),
            Duration.ofMillis(20),
            Duration.ofMillis(10))) {
            runtime.setOwnerLeaseRecoveryListener(() -> {
                republished.countDown();
                return CompletableFuture.completedFuture(null);
            });

            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();

            assertFalse(runtime.ownerLeaseHealthy());
            assertThrows(IllegalStateException.class, runtime::currentOwnerToken);
            assertEquals(1L, republished.getCount());

            store.available.set(true);

            assertTrue(republished.await(1, TimeUnit.SECONDS));
            assertTrue(runtime.ownerLeaseHealthy());
            assertEquals(1L, runtime.currentOwnerToken().leaseGeneration());
        }
    }

    @Test
    void initialClaimConflictAndGenerationExhaustionFailStartup() {
        assertInitialClaimRejected(new ZLinkOwnerLeaseClaimConflict(),
            "owner lease is already claimed");
        assertInitialClaimRejected(new ZLinkOwnerLeaseGenerationExhausted(),
            "owner lease generation is exhausted");
    }

    @Test
    void cancellingStartupReleasesAClaimThatCompletesAfterCancellation()
        throws Exception {
        DelayedClaimStore store = new DelayedClaimStore();
        try (ZLinkLocationRuntime runtime = new ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores.fromUnified(store),
            "owner-a",
            Duration.ofSeconds(1),
            Duration.ofMillis(20),
            Duration.ofMillis(100))) {
            CompletableFuture<Void> startup = runtime.start(RoutingId.from("node-a"))
                .toCompletableFuture();

            assertTrue(startup.cancel(true));
            store.completeClaim();

            assertTrue(store.released.await(1, TimeUnit.SECONDS));
            assertEquals(0, store.heartbeatRenewals.get());
        }
    }

    @Test
    void staleOwnerLeaseRenewalReclaimsANewGeneration() {
        StaleThenClaimStore store = new StaleThenClaimStore();
        AtomicBoolean republishRequested = new AtomicBoolean();
        try (ZLinkLocationRuntime runtime = new ZLinkLocationRuntime(
            store,
            "owner-a",
            Duration.ofSeconds(30),
            Duration.ofSeconds(5))) {
            runtime.setOwnerLeaseRecoveryListener(
                () -> {
                    republishRequested.set(true);
                    return CompletableFuture.completedFuture(null);
                });
            runtime.start(RoutingId.from("node-a"))
                .toCompletableFuture()
                .join();

            assertTrue(runtime.renewOwnerLeaseOnce()
                .toCompletableFuture()
                .join());
            assertTrue(runtime.ownerLeaseHealthy());
            assertEquals(2, store.claimCount.get());
            assertEquals(2, runtime.currentOwnerToken().leaseGeneration());
            assertTrue(republishRequested.get());
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

    private static void assertInitialClaimRejected(
        ZLinkOwnerLeaseClaimResult result,
        String message) {
        ZLinkLocationStoreTestAdapter store = new ZLinkLocationStoreTestAdapter() {
            @Override
            public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId,
                Duration ttl) {
                return CompletableFuture.completedFuture(result);
            }
        };
        try (ZLinkLocationRuntime runtime = new ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores.fromUnified(store),
            "owner-a",
            Duration.ofSeconds(1),
            Duration.ofSeconds(1),
            Duration.ofMillis(20))) {
            var failure = assertThrows(java.util.concurrent.CompletionException.class,
                () -> runtime.start(RoutingId.from("node-a"))
                    .toCompletableFuture().join());
            assertEquals(message, failure.getCause().getMessage());
        }
    }

    private static final class ToggleClaimStore
        extends ZLinkLocationStoreTestAdapter {
        private final AtomicBoolean available = new AtomicBoolean();
        private final AtomicInteger generation = new AtomicInteger();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
            String ownerId,
            Duration ttl) {
            if (!available.get()) {
                return new CompletableFuture<>();
            }
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseClaimed(
                new ZLinkLocationOwnerToken(ownerId, generation.incrementAndGet()),
                now.plus(ttl), now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(
            String ownerId) {
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
        }
    }

    private static final class DelayedClaimStore
        extends ZLinkLocationStoreTestAdapter {
        private final CompletableFuture<ZLinkOwnerLeaseClaimResult> claim =
            new CompletableFuture<>();
        private final CountDownLatch released = new CountDownLatch(1);
        private final AtomicInteger heartbeatRenewals = new AtomicInteger();
        private final ZLinkLocationOwnerToken token =
            new ZLinkLocationOwnerToken("owner-a", 1);
        private final AtomicBoolean claimed = new AtomicBoolean();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
            String ownerId,
            Duration ttl) {
            return claim;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(
            String ownerId) {
            if (!claimed.get()) {
                return CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
            }
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseFound(
                token, now.plusSeconds(1), now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
            ZLinkLocationOwnerToken value) {
            released.countDown();
            return CompletableFuture.completedFuture(
                ZLinkOwnerLeaseReleaseResult.RELEASED);
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
            ZLinkLocationOwnerToken value,
            Duration ttl) {
            heartbeatRenewals.incrementAndGet();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        void completeClaim() {
            Instant now = Instant.now();
            claimed.set(true);
            claim.complete(new ZLinkOwnerLeaseClaimed(
                token, now.plusSeconds(1), now));
        }
    }
}
