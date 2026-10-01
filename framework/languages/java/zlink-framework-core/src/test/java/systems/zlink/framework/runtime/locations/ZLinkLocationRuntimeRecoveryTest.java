package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewStale;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkLocationRuntimeRecoveryTest {
    @Test
    void heartbeatSnapshotKeepsItsOwnerWhenMetricsRestartTheRuntime() throws Exception {
        var generations = new AtomicInteger();
        var submittedGeneration = new java.util.concurrent.atomic.AtomicLong();
        var submitted = new CountDownLatch(1);
        var pendingRenewal = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var restarted = new AtomicBoolean();
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        Instant now = Instant.now();
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseClaimed(
                                        new ZLinkLocationOwnerToken(
                                                ownerId, generations.incrementAndGet()),
                                        now.plus(ttl),
                                        now));
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        submittedGeneration.compareAndSet(0, token.leaseGeneration());
                        submitted.countDown();
                        return pendingRenewal;
                    }

                    @Override
                    public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken token) {
                        return CompletableFuture.completedFuture(0L);
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                            ZLinkLocationOwnerToken token) {
                        return CompletableFuture.completedFuture(
                                ZLinkOwnerLeaseReleaseResult.RELEASED);
                    }
                };
        try (var runtime =
                        new ZLinkLocationRuntime(
                                ZLinkRegisteredLocationStores.fromUnified(store),
                                "owner-a",
                                Duration.ofSeconds(15),
                                Duration.ofSeconds(1),
                                Duration.ofSeconds(5),
                                Duration.ofSeconds(1));
                var metrics =
                        systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics
                                .install(
                                        new systems.zlink.framework.runtime.internal.metrics
                                                .ZLinkRuntimeMetrics.Sink() {
                                            @Override
                                            public void record(
                                                    String name,
                                                    Duration duration,
                                                    java.util.Map<String, String> tags) {
                                                if (name.equals(
                                                                "zlink.location.owner_lease.renew.lateness")
                                                        && restarted.compareAndSet(false, true)) {
                                                    runtime.stop().toCompletableFuture().join();
                                                    runtime.start(RoutingId.from("node-a"))
                                                            .toCompletableFuture()
                                                            .join();
                                                }
                                            }
                                        })) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            assertTrue(submitted.await(3, TimeUnit.SECONDS));
            assertTrue(restarted.get());
            assertEquals(
                    1L,
                    submittedGeneration.get(),
                    "the predecessor heartbeat keeps its captured owner");
            assertEquals(2L, runtime.currentOwnerToken().leaseGeneration());
        } finally {
            pendingRenewal.completeExceptionally(
                    new java.util.concurrent.CancellationException("test cleanup"));
        }
    }

    @Test
    void failedHeartbeatWaitsForPendingRenewalAndResumesAfterElapsedInterval() throws Exception {
        var firstRenewal = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var secondRenewal = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var firstStarted = new CountDownLatch(1);
        var secondStarted = new CountDownLatch(1);
        var renewals = new AtomicInteger();
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        Instant now = Instant.now();
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseClaimed(
                                        new ZLinkLocationOwnerToken(ownerId, 1),
                                        now.plus(ttl),
                                        now));
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        if (renewals.incrementAndGet() == 1) {
                            firstStarted.countDown();
                            return firstRenewal;
                        }
                        secondStarted.countDown();
                        return secondRenewal;
                    }
                };
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            assertTrue(firstStarted.await(2, TimeUnit.SECONDS));
            assertFalse(secondStarted.await(2, TimeUnit.SECONDS));
            firstRenewal.completeExceptionally(
                    new IllegalStateException("provider rejected renewal"));
            assertTrue(secondStarted.await(500, TimeUnit.MILLISECONDS));
            assertEquals(2, renewals.get());
        } finally {
            firstRenewal.completeExceptionally(
                    new java.util.concurrent.CancellationException("test cleanup"));
            secondRenewal.completeExceptionally(
                    new java.util.concurrent.CancellationException("test cleanup"));
        }
    }

    @Test
    void predecessorRecoveryFailureCannotCloseRestartedOwnerAdmission() throws Exception {
        var generation = new AtomicInteger();
        var republishStarted = new CountDownLatch(1);
        var republish = new CompletableFuture<Void>();
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        Instant now = Instant.now();
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseClaimed(
                                        new ZLinkLocationOwnerToken(
                                                ownerId, generation.incrementAndGet()),
                                        now.plus(ttl),
                                        now));
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        return CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
                    }

                    @Override
                    public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken token) {
                        return CompletableFuture.completedFuture(0L);
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                            ZLinkLocationOwnerToken token) {
                        return CompletableFuture.completedFuture(
                                ZLinkOwnerLeaseReleaseResult.RELEASED);
                    }
                };
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofHours(1),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1))) {
            runtime.setOwnerLeaseRecoveryListener(
                    () -> {
                        republishStarted.countDown();
                        return republish;
                    });
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            var previousRecovery = runtime.renewOwnerLeaseOnce().toCompletableFuture();
            assertTrue(republishStarted.await(2, TimeUnit.SECONDS));
            assertEquals(1, runtime.recoveryPreviousOwnerToken().leaseGeneration());
            runtime.stop().toCompletableFuture().join();
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            assertEquals(3, runtime.currentOwnerToken().leaseGeneration());
            assertTrue(runtime.isOwnerAdmissionOpen());
            republish.completeExceptionally(new IllegalStateException("previous recovery failed"));
            previousRecovery.get(2, TimeUnit.SECONDS);
            assertTrue(
                    runtime.isOwnerAdmissionOpen(),
                    "an earlier recovery cannot close the new owner admission");
        } finally {
            republish.completeExceptionally(
                    new java.util.concurrent.CancellationException("fixture closed"));
        }
    }

    @Test
    void stoppedOwnerRenewalCannotOverwriteTheRestartedOwnerLease() {
        Instant now = Instant.parse("2026-10-01T00:00:00Z");
        var generation = new AtomicInteger();
        var pending = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        int current = generation.incrementAndGet();
                        Instant storeNow = now.plusSeconds(current);
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseClaimed(
                                        new ZLinkLocationOwnerToken(ownerId, current),
                                        storeNow.plus(ttl),
                                        storeNow));
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        return pending;
                    }

                    @Override
                    public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken token) {
                        return CompletableFuture.completedFuture(0L);
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                            ZLinkLocationOwnerToken token) {
                        return CompletableFuture.completedFuture(
                                ZLinkOwnerLeaseReleaseResult.RELEASED);
                    }
                };
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofHours(1),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            var oldRenewal = runtime.renewOwnerLeaseOnce().toCompletableFuture();
            runtime.stop().toCompletableFuture().join();
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            Instant currentStoreNow = runtime.ownerLeaseRenewedAt();
            pending.complete(
                    new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed(
                            now.plusSeconds(15), now));
            oldRenewal.join();
            assertEquals(currentStoreNow, runtime.ownerLeaseRenewedAt());
            assertEquals(2, runtime.currentOwnerToken().leaseGeneration());
        }
    }

    @Test
    void heartbeatWaitsForPendingClaimAndStartsItsIntervalAfterCompletion() throws Exception {
        var claim = new CompletableFuture<ZLinkOwnerLeaseClaimResult>();
        var renewStarted = new CountDownLatch(1);
        var overlappingRenewal = new CountDownLatch(1);
        var releaseRenew = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var renewCount = new AtomicInteger();
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        return claim;
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        if (renewCount.incrementAndGet() > 1) {
                            overlappingRenewal.countDown();
                        }
                        renewStarted.countDown();
                        return releaseRenew;
                    }
                };
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1))) {
            var startup = runtime.start(RoutingId.from("node-a")).toCompletableFuture();
            assertFalse(renewStarted.await(2, TimeUnit.SECONDS));
            Instant now = Instant.now();
            claim.complete(
                    new ZLinkOwnerLeaseClaimed(
                            new ZLinkLocationOwnerToken("owner-a", 1), now.plusSeconds(15), now));
            startup.get(1, TimeUnit.SECONDS);
            assertFalse(
                    renewStarted.await(500, TimeUnit.MILLISECONDS),
                    "the initial claim completion starts the first renewal interval");
            assertTrue(renewStarted.await(2, TimeUnit.SECONDS));
            assertFalse(releaseRenew.isDone());
            assertFalse(overlappingRenewal.await(2, TimeUnit.SECONDS));
            assertEquals(1, renewCount.get(), "a pending renewal must not overlap");
            releaseRenew.complete(
                    new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed(
                            now.plusSeconds(15), now));
        }
    }

    @Test
    void renewalDiagnosticsPreserveTheProviderStoreNow() {
        Instant now = Instant.parse("2026-10-01T00:00:00Z");
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseClaimed(
                                        new ZLinkLocationOwnerToken(ownerId, 1),
                                        now.plus(ttl),
                                        now));
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        return CompletableFuture.completedFuture(
                                new systems.zlink.framework.runtime.internal.locations
                                        .ZLinkOwnerLeaseRenewed(now.plus(ttl), now));
                    }
                };
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(3))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            assertTrue(runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertEquals(now, runtime.ownerLeaseRenewedAt());
        }
    }

    @Test
    void unavailableInitialClaimStartsWithoutAnOwnerAndHeartbeatClaimsAfterRecovery()
            throws Exception {
        ToggleClaimStore store = new ToggleClaimStore();
        CountDownLatch republished = new CountDownLatch(1);
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(1),
                        Duration.ofMillis(20),
                        Duration.ofMillis(10),
                        Duration.ofMillis(100))) {
            runtime.setOwnerLeaseRecoveryListener(
                    () -> {
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
    void heartbeatAdoptsLateCommittedClaimAfterConflict() throws Exception {
        LateConflictStore store = new LateConflictStore();
        CountDownLatch republished = new CountDownLatch(1);
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(1),
                        Duration.ofMillis(20),
                        Duration.ofMillis(10),
                        Duration.ofMillis(100))) {
            runtime.setOwnerLeaseRecoveryListener(
                    () -> {
                        republished.countDown();
                        return CompletableFuture.completedFuture(null);
                    });

            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            assertFalse(runtime.ownerLeaseHealthy());

            store.commitLateClaim();

            assertTrue(republished.await(1, TimeUnit.SECONDS));
            assertEquals(7L, runtime.currentOwnerToken().leaseGeneration());
            assertEquals(0, store.releaseCount.get());
        }
    }

    @Test
    void exhaustedClaimDeadlineDoesNotStartConfirmationRead() {
        ExhaustedClaimStore store = new ExhaustedClaimStore();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(10),
                        Duration.ofMillis(100))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();

            assertFalse(runtime.ownerLeaseHealthy());
            assertEquals(0, store.readCount.get());
        }
    }

    @Test
    void initialClaimConflictAndGenerationExhaustionFailStartup() {
        assertInitialClaimRejected(
                new ZLinkOwnerLeaseClaimConflict(), "owner lease is already claimed");
        assertInitialClaimRejected(
                new ZLinkOwnerLeaseGenerationExhausted(), "owner lease generation is exhausted");
    }

    @Test
    void cancellingStartupReleasesAClaimThatCompletesAfterCancellation() throws Exception {
        DelayedClaimStore store = new DelayedClaimStore();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(1),
                        Duration.ofMillis(20),
                        Duration.ofMillis(100),
                        Duration.ofMillis(100))) {
            CompletableFuture<Void> startup =
                    runtime.start(RoutingId.from("node-a")).toCompletableFuture();

            assertTrue(startup.cancel(true));
            store.completeClaim();

            assertTrue(store.released.await(1, TimeUnit.SECONDS));
            assertEquals(0, store.heartbeatRenewals.get());
        }
    }

    @Test
    void storeCompletionThreadDoesNotWaitForAStateLaneTurn() throws Exception {
        DelayedClaimStore store = new DelayedClaimStore();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        Duration.ofMillis(100))) {
            CompletableFuture<Void> startup =
                    runtime.start(RoutingId.from("node-a")).toCompletableFuture();
            // A state lane turn publishes its completion on the common pool. Occupying that
            // pool makes any caller that joins a lane turn wait until the pool is released.
            int workers = ForkJoinPool.getCommonPoolParallelism();
            AtomicBoolean releasePool = new AtomicBoolean();
            CountDownLatch occupied = new CountDownLatch(workers);
            Thread completer = null;
            try {
                for (int index = 0; index < workers * 4; index++) {
                    ForkJoinPool.commonPool()
                            .execute(
                                    () -> {
                                        occupied.countDown();
                                        while (!releasePool.get()) {
                                            try {
                                                Thread.sleep(1);
                                            } catch (InterruptedException interrupted) {
                                                Thread.currentThread().interrupt();
                                                return;
                                            }
                                        }
                                    });
                }
                assertTrue(occupied.await(5, TimeUnit.SECONDS));

                completer = Thread.ofPlatform().start(store::completeClaim);
                completer.join(1_000);
                assertFalse(
                        completer.isAlive(),
                        "the Store completion thread waited for a state lane turn");
            } finally {
                releasePool.set(true);
                if (completer != null) {
                    completer.join(5_000);
                }
            }

            startup.get(5, TimeUnit.SECONDS);
            assertTrue(runtime.ownerLeaseHealthy());
        }
    }

    @Test
    void cancellationHasOneConfirmationAndReleaseOwner() throws Exception {
        CancelledLateClaimStore store = new CancelledLateClaimStore();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100),
                        Duration.ofMillis(100))) {
            CompletableFuture<Void> startup =
                    runtime.start(RoutingId.from("node-a")).toCompletableFuture();

            assertTrue(startup.cancel(true));
            assertEquals(0, store.readCount.get());
            store.completeLateClaim();

            assertTrue(store.released.await(1, TimeUnit.SECONDS));
            assertEquals(1, store.readCount.get());
            assertEquals(1, store.releaseCount.get());
            assertFalse(runtime.ownerLeaseHealthy());
            assertThrows(IllegalStateException.class, runtime::currentOwnerToken);
            assertEquals(0, store.heartbeatRenewals.get());
        }
    }

    @Test
    void cancellationPreservesCancellationWhenBoundedReleaseTimesOut() throws Exception {
        HangingReleaseStore store = new HangingReleaseStore();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(1),
                        Duration.ofMillis(20),
                        Duration.ofMillis(50),
                        Duration.ofMillis(100))) {
            CompletableFuture<Void> startup =
                    runtime.start(RoutingId.from("node-a")).toCompletableFuture();

            assertTrue(startup.cancel(true));
            store.completeClaim();

            assertThrows(java.util.concurrent.CancellationException.class, startup::join);
            assertTrue(store.releaseStarted.await(1, TimeUnit.SECONDS));
            long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
            while (!"owner lease operation timed out".equals(runtime.lastError())
                    && System.nanoTime() < until) {
                Thread.onSpinWait();
            }
            assertEquals("owner lease operation timed out", runtime.lastError());
            assertEquals(0, store.heartbeatRenewals.get());
        }
    }

    @Test
    void staleOwnerLeaseRenewalReclaimsANewGeneration() {
        StaleThenClaimStore store = new StaleThenClaimStore();
        AtomicBoolean republishRequested = new AtomicBoolean();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        store, "owner-a", Duration.ofSeconds(30), Duration.ofSeconds(5))) {
            runtime.setOwnerLeaseRecoveryListener(
                    () -> {
                        republishRequested.set(true);
                        return CompletableFuture.completedFuture(null);
                    });
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();

            assertTrue(runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertTrue(runtime.ownerLeaseHealthy());
            assertEquals(2, store.claimCount.get());
            assertEquals(2, runtime.currentOwnerToken().leaseGeneration());
            assertTrue(republishRequested.get());
        }
    }

    private static final class StaleThenClaimStore extends ZLinkLocationStoreTestAdapter {
        private final AtomicInteger claimCount = new AtomicInteger();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            int generation = claimCount.incrementAndGet();
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseClaimed(
                            new ZLinkLocationOwnerToken(ownerId, generation), now.plus(ttl), now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                ZLinkLocationOwnerToken token, Duration ttl) {
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken token) {
            return CompletableFuture.completedFuture(ZLinkOwnerLeaseReleaseResult.RELEASED);
        }
    }

    private static void assertInitialClaimRejected(
            ZLinkOwnerLeaseClaimResult result, String message) {
        ZLinkLocationStoreTestAdapter store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        return CompletableFuture.completedFuture(result);
                    }
                };
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(20),
                        Duration.ofMillis(100))) {
            var failure =
                    assertThrows(
                            java.util.concurrent.CompletionException.class,
                            () ->
                                    runtime.start(RoutingId.from("node-a"))
                                            .toCompletableFuture()
                                            .join());
            assertEquals(message, failure.getCause().getMessage());
        }
    }

    private static final class ToggleClaimStore extends ZLinkLocationStoreTestAdapter {
        private final AtomicBoolean available = new AtomicBoolean();
        private final AtomicInteger generation = new AtomicInteger();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            if (!available.get()) {
                return new CompletableFuture<>();
            }
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseClaimed(
                            new ZLinkLocationOwnerToken(ownerId, generation.incrementAndGet()),
                            now.plus(ttl),
                            now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
        }
    }

    private static final class ExhaustedClaimStore extends ZLinkLocationStoreTestAdapter {
        private final AtomicInteger readCount = new AtomicInteger();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            return new CompletableFuture<>();
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
            readCount.incrementAndGet();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
        }
    }

    private static final class LateConflictStore extends ZLinkLocationStoreTestAdapter {
        private final CompletableFuture<ZLinkOwnerLeaseClaimResult> firstClaim =
                new CompletableFuture<>();
        private final AtomicInteger claimCount = new AtomicInteger();
        private final AtomicInteger releaseCount = new AtomicInteger();
        private final ZLinkLocationOwnerToken token = new ZLinkLocationOwnerToken("owner-a", 7);
        private final AtomicBoolean committed = new AtomicBoolean();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            if (claimCount.getAndIncrement() == 0) {
                return firstClaim;
            }
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseClaimConflict());
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
            if (!committed.get()) {
                return CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
            }
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseFound(token, now.plusSeconds(1), now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken value) {
            releaseCount.incrementAndGet();
            return CompletableFuture.completedFuture(ZLinkOwnerLeaseReleaseResult.RELEASED);
        }

        void commitLateClaim() {
            Instant now = Instant.now();
            committed.set(true);
            firstClaim.complete(new ZLinkOwnerLeaseClaimed(token, now.plusSeconds(1), now));
        }
    }

    private static final class DelayedClaimStore extends ZLinkLocationStoreTestAdapter {
        private final CompletableFuture<ZLinkOwnerLeaseClaimResult> claim =
                new CompletableFuture<>();
        private final CountDownLatch released = new CountDownLatch(1);
        private final AtomicInteger heartbeatRenewals = new AtomicInteger();
        private final ZLinkLocationOwnerToken token = new ZLinkLocationOwnerToken("owner-a", 1);
        private final AtomicBoolean claimed = new AtomicBoolean();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            return claim;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
            if (!claimed.get()) {
                return CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
            }
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseFound(token, now.plusSeconds(1), now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken value) {
            released.countDown();
            return CompletableFuture.completedFuture(ZLinkOwnerLeaseReleaseResult.RELEASED);
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                ZLinkLocationOwnerToken value, Duration ttl) {
            heartbeatRenewals.incrementAndGet();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        void completeClaim() {
            Instant now = Instant.now();
            claimed.set(true);
            claim.complete(new ZLinkOwnerLeaseClaimed(token, now.plusSeconds(1), now));
        }
    }

    private static final class CancelledLateClaimStore extends ZLinkLocationStoreTestAdapter {
        private final CompletableFuture<ZLinkOwnerLeaseClaimResult> claim =
                new CompletableFuture<>();
        private final CountDownLatch released = new CountDownLatch(1);
        private final AtomicInteger readCount = new AtomicInteger();
        private final AtomicInteger releaseCount = new AtomicInteger();
        private final AtomicInteger heartbeatRenewals = new AtomicInteger();
        private final ZLinkLocationOwnerToken token = new ZLinkLocationOwnerToken("owner-a", 1);

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            return claim;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
            readCount.incrementAndGet();
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseFound(token, now.plusSeconds(1), now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken value) {
            releaseCount.incrementAndGet();
            released.countDown();
            return CompletableFuture.completedFuture(ZLinkOwnerLeaseReleaseResult.RELEASED);
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                ZLinkLocationOwnerToken value, Duration ttl) {
            heartbeatRenewals.incrementAndGet();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        void completeLateClaim() {
            Instant now = Instant.now();
            claim.complete(new ZLinkOwnerLeaseClaimed(token, now.plusSeconds(1), now));
        }
    }

    private static final class HangingReleaseStore extends ZLinkLocationStoreTestAdapter {
        private final CompletableFuture<ZLinkOwnerLeaseClaimResult> claim =
                new CompletableFuture<>();
        private final CountDownLatch releaseStarted = new CountDownLatch(1);
        private final AtomicInteger heartbeatRenewals = new AtomicInteger();
        private final ZLinkLocationOwnerToken token = new ZLinkLocationOwnerToken("owner-a", 1);

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            return claim;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseFound(token, now.plusSeconds(1), now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken value) {
            releaseStarted.countDown();
            return new CompletableFuture<>();
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                ZLinkLocationOwnerToken value, Duration ttl) {
            heartbeatRenewals.incrementAndGet();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        void completeClaim() {
            Instant now = Instant.now();
            claim.complete(new ZLinkOwnerLeaseClaimed(token, now.plusSeconds(1), now));
        }
    }
}
