package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
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
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkLocationRuntimeRecoveryTest {
    @Test
    void descriptorPublicationDeadlineEndsOnlyTheWaitAndNextRenewalCanRepublish() throws Exception {
        var storeWrite = new CompletableFuture<Void>();
        var writes = new AtomicInteger();
        var clock = new AtomicLong(1L);
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var timeout = Duration.ofMillis(50);
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(
                                new ZLinkLocationStoreTestAdapter() {}),
                        "publication-owner",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        timeout,
                        Duration.ZERO,
                        clock::get,
                        scheduler)) {
            var scheduled = scheduler.nextTask();
            var publication =
                    runtime.writeDescriptor(
                                    () -> {
                                        writes.incrementAndGet();
                                        return storeWrite;
                                    })
                            .toCompletableFuture();
            assertFalse(publication.isDone(), "publication is an asynchronous continuation");
            var deadline = scheduled.get(1, TimeUnit.SECONDS);
            assertEquals(timeout.toNanos(), deadline.getDelay(TimeUnit.NANOSECONDS));
            clock.addAndGet(timeout.toNanos());
            deadline.run();
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> publication.get(1, TimeUnit.SECONDS));
            assertTrue(failure.getCause() instanceof java.util.concurrent.TimeoutException);
            assertFalse(storeWrite.isDone(), "the deadline ends the caller wait only");
            runtime.writeDescriptor(
                            () -> {
                                writes.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            })
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            storeWrite.complete(null);
            assertTrue(publication.isCompletedExceptionally());
            assertEquals(2, writes.get(), "late completion must not submit another publication");
        }
    }

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
            runtime.metricScopes(
                    java.util.List.of(
                            java.util.Map.of("scope_kind", "mesh", "scope_name", "test-mesh")));
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
                    admissionWasOpen -> {
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
    void successfulStartupStoppedByCompletionObserverDoesNotRunCancellationCleanup() {
        var claim = new CompletableFuture<ZLinkOwnerLeaseClaimResult>();
        var removed = new AtomicInteger();
        var released = new AtomicInteger();
        var token = new ZLinkLocationOwnerToken("owner-a", 1);
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        return claim;
                    }

                    @Override
                    public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken owner) {
                        assertEquals(token, owner);
                        removed.incrementAndGet();
                        return CompletableFuture.completedFuture(0L);
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                            ZLinkLocationOwnerToken owner) {
                        assertEquals(token, owner);
                        released.incrementAndGet();
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
            var startup = runtime.start(RoutingId.from("node-a")).toCompletableFuture();
            // Stop from the success observer before the earlier cleanup observer runs.
            var stopped = startup.thenApply(ignored -> runtime.stop()).thenCompose(stage -> stage);
            Instant now = Instant.now();
            claim.complete(new ZLinkOwnerLeaseClaimed(token, now.plusSeconds(15), now));
            startup.join();
            stopped.join();
            assertEquals(1, removed.get());
            assertEquals(1, released.get());
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
    void heartbeatWaitsForPendingClaimAndNeverOverlapsRenewals() throws Exception {
        var claim = new CompletableFuture<ZLinkOwnerLeaseClaimResult>();
        var releaseRenew = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var renewCount = new AtomicInteger();
        var renewStarted = new CompletableFuture<Void>();
        var clock = new java.util.concurrent.atomic.AtomicLong(Duration.ofSeconds(1).toNanos());
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
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
                        renewCount.incrementAndGet();
                        renewStarted.complete(null);
                        return releaseRenew;
                    }
                };
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofMillis(100),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1),
                        clock::get,
                        scheduler)) {
            var startup = runtime.start(RoutingId.from("node-a")).toCompletableFuture();
            clock.addAndGet(Duration.ofMillis(150).toNanos());
            assertFalse(startup.isDone());
            assertEquals(0, renewCount.get());
            assertEquals(1, scheduler.tasks.size(), "only the claim timeout is scheduled");
            assertEquals(5000, scheduler.tasks.getFirst().getDelay(TimeUnit.MILLISECONDS));
            Instant now = Instant.now();
            claim.complete(
                    new ZLinkOwnerLeaseClaimed(
                            new ZLinkLocationOwnerToken("owner-a", 1), now.plusSeconds(15), now));
            startup.get(1, TimeUnit.SECONDS);
            // The owner deadline task (admission lifetime) is scheduled with the heartbeat.
            var heartbeat =
                    scheduler.tasks.stream()
                            .filter(task -> task.getDelay(TimeUnit.MILLISECONDS) < 1000)
                            .reduce((first, second) -> second)
                            .orElseThrow();
            // Location section 5: the initial claim already consumed the renewal interval.
            assertEquals(0, heartbeat.getDelay(TimeUnit.MILLISECONDS));
            heartbeat.run();
            renewStarted.join();
            assertEquals(1, renewCount.get());
            clock.addAndGet(Duration.ofSeconds(1).toNanos());
            assertFalse(releaseRenew.isDone());
            assertEquals(
                    0,
                    scheduler.tasks.stream()
                            .filter(task -> !task.isDone())
                            // Owner admission and renew timeout tasks are not heartbeat tasks.
                            .filter(task -> task.getDelay(TimeUnit.MILLISECONDS) < 1000)
                            .count(),
                    "a pending renewal must not schedule an overlapping renewal");
            var scheduled = scheduler.nextTask(task -> task.getDelay(TimeUnit.MILLISECONDS) < 1000);
            releaseRenew.complete(
                    new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed(
                            now.plusSeconds(15), now));
            var next = scheduled.join();
            assertEquals(1, renewCount.get());
            assertFalse(next.isDone());
            assertEquals(0, next.getDelay(TimeUnit.MILLISECONDS));
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
                    admissionWasOpen -> {
                        republished.countDown();
                        return CompletableFuture.completedFuture(null);
                    });

            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();

            assertFalse(runtime.ownerLeaseHealthy());
            assertOwnerAdmissionClosed(runtime);
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
                    admissionWasOpen -> {
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
    void completedClaimRejectionSurvivesExpiredConfirmationDeadline() {
        for (var result :
                java.util.List.of(
                        new ZLinkOwnerLeaseClaimConflict(),
                        new ZLinkOwnerLeaseGenerationExhausted())) {
            var now = new java.util.concurrent.atomic.AtomicLong();
            var timeout = Duration.ofSeconds(1);
            var reads = new AtomicInteger();
            var store =
                    new ZLinkLocationStoreTestAdapter() {
                        @Override
                        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                                String ownerId, Duration ttl) {
                            now.addAndGet(timeout.toNanos());
                            return CompletableFuture.completedFuture(result);
                        }

                        @Override
                        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(
                                String ownerId) {
                            reads.incrementAndGet();
                            return CompletableFuture.failedFuture(
                                    new java.util.concurrent.TimeoutException(
                                            "confirmation failed"));
                        }
                    };
            try (var runtime =
                    new ZLinkLocationRuntime(
                            ZLinkRegisteredLocationStores.fromUnified(store),
                            "owner-a",
                            Duration.ofSeconds(15),
                            Duration.ofSeconds(5),
                            timeout,
                            Duration.ofSeconds(1),
                            now::get,
                            java.util.concurrent.Executors.newSingleThreadScheduledExecutor())) {
                var failure =
                        assertThrows(
                                java.util.concurrent.CompletionException.class,
                                () ->
                                        runtime.start(RoutingId.from("node-a"))
                                                .toCompletableFuture()
                                                .join());
                assertEquals(
                        result instanceof ZLinkOwnerLeaseGenerationExhausted
                                ? "owner lease generation is exhausted"
                                : "owner lease is already claimed",
                        failure.getCause().getMessage());
                assertFalse(runtime.ownerLeaseHealthy());
                assertEquals(0, reads.get(), "confirmation must not start after the deadline");
            }
        }
    }

    @Test
    void cancellingStartupReleasesAClaimThatCompletesAfterRenewTimeout() throws Exception {
        var store = new DelayedClaimStore();
        var clock = new AtomicLong(1L);
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var renewTimeout = Duration.ofMillis(100);
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        renewTimeout,
                        Duration.ZERO,
                        clock::get,
                        scheduler)) {
            var startup = runtime.start(RoutingId.from("node-a")).toCompletableFuture();
            assertTrue(startup.cancel(true));
            clock.addAndGet(renewTimeout.plusNanos(1).toNanos());
            var stopping = runtime.stop().toCompletableFuture();
            assertFalse(stopping.isDone(), "Stop must observe the pending claim");
            store.release.complete(ZLinkOwnerLeaseReleaseResult.RELEASED);
            store.completeClaim();
            stopping.get(5, TimeUnit.SECONDS);
            assertEquals(1, store.readCount.get());
            assertEquals(1, store.releaseCount.get());
            assertFalse(runtime.ownerLeaseHealthy());
            assertOwnerAdmissionClosed(runtime);
            assertEquals(0, store.heartbeatRenewals.get());
            assertTrue(scheduler.tasks.stream().allMatch(Future::isDone), "No pending heartbeat");
        }
    }

    @Test
    void cancelledStartupUsesOneStopForClaimCleanupAndRepeatedStopDoesNotCallStore()
            throws Exception {
        var store = new DelayedClaimStore();
        var clock = new AtomicLong(1L);
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100),
                        Duration.ZERO,
                        clock::get,
                        scheduler)) {
            var startup = runtime.start(RoutingId.from("node-a")).toCompletableFuture();
            store.claimCalled.get(1, TimeUnit.SECONDS);
            assertTrue(startup.cancel(true));
            store.completeClaim();
            store.releaseCalled.get(1, TimeUnit.SECONDS);

            var stopping = runtime.stop().toCompletableFuture();
            assertFalse(stopping.isDone(), "Stop must wait for the in-flight claim cleanup");
            store.release.complete(ZLinkOwnerLeaseReleaseResult.RELEASED);
            stopping.get(5, TimeUnit.SECONDS);

            assertThrows(java.util.concurrent.CancellationException.class, startup::join);
            int storeCallsAfterCleanup = store.storeCalls.get();
            var repeatedStop = runtime.stop().toCompletableFuture();
            assertTrue(repeatedStop.isDone());
            repeatedStop.get(1, TimeUnit.SECONDS);
            assertEquals(
                    storeCallsAfterCleanup,
                    store.storeCalls.get(),
                    "Repeated Stop must not call the Store");
        }
    }

    @Test
    void stopDeadlineAbandonsPendingClaimWithoutCallingStore() throws Exception {
        var store = new DelayedClaimStore();
        var clock = new AtomicLong(1L);
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var records = new AtomicInteger();
        var logger = Logger.getLogger(ZLinkLocationRuntime.class.getName());
        var listener =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        if ("location.startup_claim_cleanup_abandoned"
                                .equals(record.getMessage())) {
                            records.incrementAndGet();
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(listener);
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100),
                        Duration.ZERO,
                        clock::get,
                        scheduler)) {
            Instant stopDeadline = Instant.now().plusSeconds(10);
            var startup =
                    runtime.start(RoutingId.from("node-a"), () -> stopDeadline)
                            .toCompletableFuture();
            var expiry = scheduler.nextTask();
            assertTrue(startup.cancel(true));
            var stopping = runtime.stop(stopDeadline).toCompletableFuture();
            assertFalse(stopping.isDone());
            var timer = expiry.get(5, TimeUnit.SECONDS);
            clock.addAndGet(timer.getDelay(TimeUnit.NANOSECONDS));
            timer.run();
            var failure =
                    assertThrows(ExecutionException.class, () -> stopping.get(5, TimeUnit.SECONDS));
            assertTrue(failure.getCause() instanceof TimeoutException);

            store.completeClaim();
            assertEquals(0, store.readCount.get());
            assertEquals(0, store.releaseCount.get());
            assertEquals(0, store.heartbeatRenewals.get());
            assertTrue(scheduler.tasks.stream().allMatch(Future::isDone), "No pending heartbeat");
            assertEquals(1, records.get());
        } finally {
            logger.removeHandler(listener);
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
    void stopPreservesCancelledStartupReleaseFailure() throws Exception {
        var store = new DelayedClaimStore();
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var clock = new AtomicLong(1L);
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100),
                        Duration.ZERO,
                        clock::get,
                        scheduler)) {
            var startup = runtime.start(RoutingId.from("node-a")).toCompletableFuture();
            var stopping = runtime.stop(Instant.now().plusSeconds(10)).toCompletableFuture();
            var releaseTimeout = scheduler.nextTask();
            store.completeClaim();
            var timer = releaseTimeout.get(5, TimeUnit.SECONDS);
            clock.addAndGet(timer.getDelay(TimeUnit.NANOSECONDS));
            timer.run();

            var failure =
                    assertThrows(ExecutionException.class, () -> stopping.get(5, TimeUnit.SECONDS));
            assertTrue(failure.getCause() instanceof TimeoutException);
            assertThrows(java.util.concurrent.CancellationException.class, startup::join);
            assertEquals("owner lease operation timed out", runtime.lastError());
            assertEquals(1, store.readCount.get());
            assertEquals(1, store.releaseCount.get());
            assertEquals(0, store.heartbeatRenewals.get());
        }
    }

    @Test
    void oldStartupCleanupPreservesTheNewLifecycleLeaseAndError() throws Exception {
        for (boolean providerFailure : new boolean[] {false, true}) {
            RestartedClaimStore store = new RestartedClaimStore();
            try (ZLinkLocationRuntime runtime =
                    new ZLinkLocationRuntime(
                            ZLinkRegisteredLocationStores.fromUnified(store),
                            "owner-a",
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(1),
                            Duration.ofSeconds(1),
                            Duration.ofMillis(100))) {
                var first = runtime.start(RoutingId.from("node-a")).toCompletableFuture();
                assertTrue(first.cancel(true));
                var oldStopping = runtime.stop().toCompletableFuture();
                runtime.start(RoutingId.from("node-b"))
                        .toCompletableFuture()
                        .get(1, TimeUnit.SECONDS);
                if (providerFailure) {
                    store.firstClaim.completeExceptionally(
                            new IllegalStateException("old provider failed"));
                } else {
                    Instant now = Instant.now();
                    store.firstClaim.complete(
                            new ZLinkOwnerLeaseClaimed(
                                    new ZLinkLocationOwnerToken("owner-a", 1),
                                    now.plusSeconds(5),
                                    now));
                }
                oldStopping.get(1, TimeUnit.SECONDS);
                assertEquals(store.current, runtime.currentOwnerToken());
                assertEquals(0, store.releases.get());
                assertEquals(null, runtime.lastError());
                runtime.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
                assertEquals(1, store.releases.get());
            }
        }
    }

    @Test
    void normalDirectStopReleasesAnOwnerAfterStartupRenewTimeout() throws Exception {
        RestartedClaimStore store = new RestartedClaimStore();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100),
                        Duration.ofMillis(100))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertFalse(runtime.ownerLeaseHealthy());
            assertTrue(
                    runtime.renewOwnerLeaseOnce().toCompletableFuture().get(1, TimeUnit.SECONDS));
            Instant now = Instant.now();
            store.firstClaim.complete(
                    new ZLinkOwnerLeaseClaimed(
                            new ZLinkLocationOwnerToken("owner-a", 1), now.plusSeconds(5), now));
            runtime.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertEquals(1, store.releases.get());
        }
    }

    @Test
    void normalDirectStopWaitsForThePendingInitialClaim() throws Exception {
        RestartedClaimStore store = new RestartedClaimStore();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100),
                        Duration.ofMillis(100))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertFalse(runtime.ownerLeaseHealthy());
            assertTrue(
                    runtime.renewOwnerLeaseOnce().toCompletableFuture().get(1, TimeUnit.SECONDS));
            var stopping = runtime.stop().toCompletableFuture();
            assertFalse(stopping.isDone());
            store.firstClaim.completeExceptionally(
                    new IllegalStateException("initial claim failed"));
            stopping.get(1, TimeUnit.SECONDS);
            assertEquals(1, store.releases.get());
            assertFalse(runtime.ownerLeaseHealthy());
        }
    }

    @Test
    void successfulStartupLeavesTheRecoveredTokenToStopWithoutCancellationConfirmation()
            throws Exception {
        for (boolean failConfirmation : new boolean[] {false, true}) {
            RestartedClaimStore store = new RestartedClaimStore();
            try (ZLinkLocationRuntime runtime =
                    new ZLinkLocationRuntime(
                            ZLinkRegisteredLocationStores.fromUnified(store),
                            "owner-a",
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(1),
                            Duration.ofMillis(100),
                            Duration.ofMillis(100))) {
                Instant deadline =
                        Instant.now()
                                .plus(
                                        systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
                                                .DEFAULT_TERMINATION_DEADLINE);
                runtime.start(RoutingId.from("node-a"), () -> deadline)
                        .toCompletableFuture()
                        .get(1, TimeUnit.SECONDS);
                assertTrue(
                        runtime.renewOwnerLeaseOnce()
                                .toCompletableFuture()
                                .get(1, TimeUnit.SECONDS));
                int readsBeforeStop = store.reads.get();
                store.failRead = failConfirmation;
                var stopping = runtime.stop(deadline).toCompletableFuture();
                store.firstClaim.completeExceptionally(
                        new IllegalStateException("initial provider failed"));
                stopping.get(1, TimeUnit.SECONDS);
                assertEquals(readsBeforeStop, store.reads.get());
                assertEquals(1, store.releases.get());
                assertFalse(runtime.ownerLeaseHealthy());
            }
        }
    }

    @Test
    void repeatedStopObservesTheSameOwnerCleanupWithoutResubmittingIt() throws Exception {
        RestartedClaimStore store = new RestartedClaimStore();
        store.releaseResult = new CompletableFuture<>();
        Instant now = Instant.now();
        store.firstClaim.complete(
                new ZLinkOwnerLeaseClaimed(store.current, now.plusSeconds(5), now));
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().get(1, TimeUnit.SECONDS);
            var first = runtime.stop().toCompletableFuture();
            assertTrue(store.releaseCalled.await(1, TimeUnit.SECONDS));
            var second = runtime.stop().toCompletableFuture();
            org.junit.jupiter.api.Assertions.assertSame(first, second);
            assertFalse(second.isDone());
            assertEquals(1, store.releases.get());
            store.releaseResult.complete(ZLinkOwnerLeaseReleaseResult.RELEASED);
            first.get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void normalStopPreservesReleaseFailureWithoutCancellationConfirmation() throws Exception {
        RestartedClaimStore store = new RestartedClaimStore();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100),
                        Duration.ofMillis(100))) {
            Instant deadline =
                    Instant.now()
                            .plus(
                                    systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
                                            .DEFAULT_TERMINATION_DEADLINE);
            runtime.start(RoutingId.from("node-a"), () -> deadline)
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            assertTrue(
                    runtime.renewOwnerLeaseOnce().toCompletableFuture().get(1, TimeUnit.SECONDS));
            int readsBeforeStop = store.reads.get();
            store.failRead = true;
            store.releaseResult =
                    CompletableFuture.failedFuture(new IllegalStateException("release failed"));
            var stopping = runtime.stop(deadline).toCompletableFuture();
            store.firstClaim.completeExceptionally(
                    new IllegalStateException("initial provider failed"));
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> stopping.get(1, TimeUnit.SECONDS));
            assertEquals("release failed", failure.getCause().getMessage());
            assertEquals(0, failure.getCause().getSuppressed().length);
            assertEquals(readsBeforeStop, store.reads.get());
            assertEquals(1, store.releases.get());
        }
    }

    @Test
    void confirmedInitialClaimFailureLeavesSuccessfulStartupDiagnosticsHealthy() throws Exception {
        RestartedClaimStore store = new RestartedClaimStore();
        store.firstClaim.completeExceptionally(
                new IllegalStateException("claim outcome uncertain"));
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(100))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertTrue(runtime.ownerLeaseHealthy());
            assertEquals(store.current, runtime.currentOwnerToken());
            assertEquals(null, runtime.lastError());
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
                    admissionWasOpen -> {
                        republishRequested.set(true);
                        return CompletableFuture.completedFuture(null);
                    });
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();

            AtomicInteger disposed = new AtomicInteger();
            var execution = runtime.execution(() -> false, disposed::incrementAndGet);
            execution.ready().toCompletableFuture().join();
            store.onClaim = () -> assertEquals(1, disposed.get(), "terminal before new claim");
            assertTrue(runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertTrue(runtime.ownerLeaseHealthy());
            assertEquals(2, store.claimCount.get());
            assertEquals(2, runtime.currentOwnerToken().leaseGeneration());
            assertTrue(republishRequested.get());
        }
    }

    private static final class StaleThenClaimStore extends ZLinkLocationStoreTestAdapter {
        private final AtomicInteger claimCount = new AtomicInteger();
        Runnable onClaim;

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            int generation = claimCount.incrementAndGet();
            if (onClaim != null) onClaim.run();
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

    private static final class RestartedClaimStore extends ZLinkLocationStoreTestAdapter {
        private final CompletableFuture<ZLinkOwnerLeaseClaimResult> firstClaim =
                new CompletableFuture<>();
        private final ZLinkLocationOwnerToken current = new ZLinkLocationOwnerToken("owner-a", 2);
        private final AtomicInteger claims = new AtomicInteger();
        private final AtomicInteger releases = new AtomicInteger();
        private final AtomicInteger reads = new AtomicInteger();
        private boolean failRead;
        private final CountDownLatch releaseCalled = new CountDownLatch(1);
        private CompletableFuture<ZLinkOwnerLeaseReleaseResult> releaseResult =
                CompletableFuture.completedFuture(ZLinkOwnerLeaseReleaseResult.RELEASED);

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            if (claims.getAndIncrement() == 0) return firstClaim;
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseClaimed(current, now.plus(ttl), now));
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
            reads.incrementAndGet();
            if (failRead)
                return CompletableFuture.failedFuture(
                        new IllegalStateException("confirmation failed"));
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseFound(current, now.plusSeconds(5), now));
        }

        @Override
        public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken token) {
            assertEquals(current, token);
            return CompletableFuture.completedFuture(0L);
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken token) {
            assertEquals(current, token);
            releases.incrementAndGet();
            releaseCalled.countDown();
            return releaseResult;
        }
    }

    private static final class DelayedClaimStore extends ZLinkLocationStoreTestAdapter {
        private final CompletableFuture<ZLinkOwnerLeaseClaimResult> claim =
                new CompletableFuture<>();
        private final CompletableFuture<Void> claimCalled = new CompletableFuture<>();
        private final CompletableFuture<Void> releaseCalled = new CompletableFuture<>();
        private final AtomicInteger storeCalls = new AtomicInteger();
        private final AtomicInteger readCount = new AtomicInteger();
        private final AtomicInteger releaseCount = new AtomicInteger();
        private final AtomicInteger heartbeatRenewals = new AtomicInteger();
        private final ZLinkLocationOwnerToken token = new ZLinkLocationOwnerToken("owner-a", 1);
        private final CompletableFuture<ZLinkOwnerLeaseReleaseResult> release =
                new CompletableFuture<>();

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            storeCalls.incrementAndGet();
            claimCalled.complete(null);
            return claim;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String ownerId) {
            storeCalls.incrementAndGet();
            readCount.incrementAndGet();
            CompletionStage<ZLinkOwnerLeaseReadResult> result;
            if (!claim.isDone()) {
                result = CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
            } else {
                Instant now = Instant.now();
                result =
                        CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseFound(token, now.plusSeconds(1), now));
            }
            return result;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken value) {
            storeCalls.incrementAndGet();
            releaseCount.incrementAndGet();
            releaseCalled.complete(null);
            return release;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                ZLinkLocationOwnerToken value, Duration ttl) {
            storeCalls.incrementAndGet();
            heartbeatRenewals.incrementAndGet();
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        void completeClaim() {
            Instant now = Instant.now();
            claim.complete(new ZLinkOwnerLeaseClaimed(token, now.plusSeconds(1), now));
        }
    }

    /** Spec 01-submit-and-completion: a runtime that takes no new admission fails ShuttingDown. */
    private static void assertOwnerAdmissionClosed(ZLinkLocationRuntime runtime) {
        ZLinkFrameworkException closed =
                assertThrows(ZLinkFrameworkException.class, runtime::currentOwnerToken);
        assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, closed.kind());
        assertArrayEquals(
                new int[] {
                    RequestResult.TERMINATED.value(),
                    (int) ServiceWireConstants.FRAMEWORK_ERROR_NONE
                },
                ZLinkRequestFailureMapping.outgoing(new CompletionException(closed)));
    }
}
