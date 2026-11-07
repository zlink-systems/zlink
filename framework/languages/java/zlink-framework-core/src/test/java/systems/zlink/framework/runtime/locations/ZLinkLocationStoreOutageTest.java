package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

public final class ZLinkLocationStoreOutageTest {
    private static final Duration TTL = Duration.ofSeconds(30);
    private static final Duration MARGIN = Duration.ofSeconds(5);

    @Test
    void optionIsFalseByDefaultAndRuntimeCapturesItBeforeStartup() {
        var options = new ZLinkLocationOptions();
        assertFalse(options.storeOutageRetention());
        options.setStoreOutageRetention(true);
        assertTrue(options.storeOutageRetention());
        try (var fixture = new Fixture(false, options.storeOutageRetention())) {
            options.setStoreOutageRetention(false);
            fixture.failRenewal();
            assertEquals(
                    100L + Duration.ofSeconds(40).toNanos(),
                    fixture.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            assertThrows(
                    IllegalStateException.class,
                    () -> fixture.runtime.setStoreOutageRetention(false));
        }
    }

    @RepeatedTest(100)
    void outageContract() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean retention : new boolean[] {false, true}) {
                boundaryAndLateResponses(provider, retention);
                recoveryClosesEpisode(provider, retention);
                definitiveResponsesDoNotOpenEpisode(provider, retention);
                readFailureOpensEpisode(provider, retention);
                renewTimeoutResultOpensEpisode(provider, retention);
                successfulRenewalRequestStartIsTheBasis(provider, retention);
                successfulClaimRequestStartIsTheBasis(provider, retention);
                successAtCutoffCannotCloseEpisode(provider, retention);
                synchronousProviderErrorsOpenEpisode(provider, retention);
            }
        }
    }

    private void boundaryAndLateResponses(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            long basis = f.now.get();
            var late = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
            f.store.response = late;
            var pending = f.runtime.renewOwnerLeaseOnce();
            f.now.addAndGet(1);
            f.failRenewal();
            var opened = f.runtime.ownerLeaseSnapshot();
            long cutoff = basis + Duration.ofSeconds(retention ? 40 : 25).toNanos();
            assertEquals(f.now.get(), opened.outageObservedAtNanos());
            assertEquals(cutoff, opened.outageCutoffNanos());
            assertEquals(
                    cutoff - f.now.get(),
                    f.scheduler.tasks.getLast().getDelay(TimeUnit.NANOSECONDS));
            f.now.addAndGet(1);
            f.failRenewal();
            assertEquals(
                    opened.outageObservedAtNanos(),
                    f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            // Stale during an outage cannot reclaim before normal expiration.
            f.store.response = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
            f.store.claimResponse =
                    CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseClaimed(
                                    new ZLinkLocationOwnerToken("outage-owner", 2),
                                    Instant.EPOCH.plus(TTL),
                                    Instant.EPOCH));
            assertFalse(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertEquals(1, f.runtime.ownerLeaseSnapshot().token().leaseGeneration());
            assertEquals(cutoff, f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            late.complete(new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plus(TTL), Instant.EPOCH));
            assertFalse(pending.toCompletableFuture().join());
            assertEquals(cutoff, f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            f.now.set(cutoff - 1);
            assertFalse(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            f.now.set(cutoff);
            f.fireCutoff();
            assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            assertEquals(
                    false,
                    f.runtime.isOwnerAdmissionOpen(),
                    "normal expiration has not permitted a new claim");
            // No mesh or application turn exists. A later success cannot clear the latch.
            f.store.response =
                    CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plus(TTL), Instant.EPOCH));
            f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join();
            assertTrue(f.runtime.isOwnerAdmissionOpen(), "existing renewal admission is unchanged");
            assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            assertEquals(cutoff, f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
        }
    }

    private void recoveryClosesEpisode(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.now.addAndGet(1);
            f.failRenewal();
            long cutoff = f.runtime.ownerLeaseSnapshot().outageCutoffNanos();
            f.now.addAndGet(1);
            f.store.response = null;
            assertTrue(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertNull(f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            f.now.set(cutoff);
            f.fireCutoff();
            assertFalse(f.runtime.ownerLeaseSnapshot().outageDiscarded());
        }
    }

    private void definitiveResponsesDoNotOpenEpisode(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.store.response = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
            f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join();
            assertNull(f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            f.store.claimResponse =
                    CompletableFuture.completedFuture(new ZLinkOwnerLeaseClaimConflict());
            f.store.readResponse = CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
            f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join();
            assertNull(f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            f.store.readResponse = null;
        }
    }

    private void readFailureOpensEpisode(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.store.response = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
            f.store.claimResponse =
                    CompletableFuture.completedFuture(new ZLinkOwnerLeaseClaimConflict());
            f.store.readResponse =
                    CompletableFuture.failedFuture(new TimeoutException("read timed out"));
            f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join();
            assertEquals(f.now.get(), f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            assertEquals(
                    100L + Duration.ofSeconds(retention ? 40 : 25).toNanos(),
                    f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            f.store.readResponse = null;
        }
    }

    private void renewTimeoutResultOpensEpisode(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            var response = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
            f.store.response = response;
            var renewal = f.runtime.renewOwnerLeaseOnce();
            assertFalse(renewal.toCompletableFuture().isDone());
            f.now.addAndGet(Duration.ofSeconds(3).toNanos());
            response.completeExceptionally(new TimeoutException("provider renew timed out"));
            assertFalse(renewal.toCompletableFuture().join());
            assertEquals(f.now.get(), f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            assertFalse(f.runtime.ownerLeaseSnapshot().outageDiscarded());
        }
    }

    private void successfulRenewalRequestStartIsTheBasis(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.now.addAndGet(Duration.ofSeconds(5).toNanos());
            long requestStart = f.now.get();
            var response = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
            f.store.response = response;
            var renewal = f.runtime.renewOwnerLeaseOnce();
            f.now.addAndGet(Duration.ofSeconds(1).toNanos());
            response.complete(new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plus(TTL), Instant.EPOCH));
            renewal.toCompletableFuture().join();
            f.failRenewal();
            assertEquals(
                    requestStart,
                    f.runtime.ownerLeaseSnapshot().lastSuccessfulRequestStartedNanos());
            assertEquals(
                    requestStart + Duration.ofSeconds(retention ? 40 : 25).toNanos(),
                    f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
        }
    }

    private void successfulClaimRequestStartIsTheBasis(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.now.addAndGet(Duration.ofSeconds(5).toNanos());
            long requestStart = f.now.get();
            f.store.response = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
            f.store.claimResponse =
                    CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseClaimed(
                                    new ZLinkLocationOwnerToken("outage-owner", 2),
                                    Instant.EPOCH.plus(TTL),
                                    Instant.EPOCH));
            assertTrue(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertNull(f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            f.now.addAndGet(Duration.ofSeconds(1).toNanos());
            f.failRenewal();
            assertEquals(
                    requestStart,
                    f.runtime.ownerLeaseSnapshot().lastSuccessfulRequestStartedNanos());
            assertEquals(
                    requestStart + Duration.ofSeconds(retention ? 40 : 25).toNanos(),
                    f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
        }
    }

    private void successAtCutoffCannotCloseEpisode(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.failRenewal();
            long cutoff = f.runtime.ownerLeaseSnapshot().outageCutoffNanos();
            f.now.set(cutoff - 1);
            var response = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
            f.store.response = response;
            var renewal = f.runtime.renewOwnerLeaseOnce();
            f.now.set(cutoff);
            response.complete(new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plus(TTL), Instant.EPOCH));
            renewal.toCompletableFuture().join();
            assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            assertEquals(cutoff, f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
        }
    }

    private void synchronousProviderErrorsOpenEpisode(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.store.synchronousRenewFailure = true;
            assertThrows(
                    CompletionException.class,
                    () -> f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertEquals(f.now.get(), f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
        }
        try (var f = new Fixture(provider, retention)) {
            f.store.response = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
            f.store.claimResponse =
                    CompletableFuture.completedFuture(new ZLinkOwnerLeaseClaimConflict());
            f.store.synchronousReadFailure = true;
            assertFalse(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertEquals(f.now.get(), f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            f.store.synchronousReadFailure = false;
        }
    }

    @RepeatedTest(100)
    void ordinaryTurnsDoNotReadClockAndExpiredAdmissionStillBlocksNewTurns() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean retention : new boolean[] {false, true}) {
                try (var f = new Fixture(provider, retention)) {
                    var spot =
                            new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                                    Runnable::run,
                                    systems.zlink.framework.execution.ZLinkExecutionLanePolicy
                                            .spot());
                    var actor =
                            new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                                    Runnable::run,
                                    systems.zlink.framework.execution.ZLinkExecutionLanePolicy
                                            .actorDelivery());
                    var timer =
                            systems.zlink.framework.execution.ZLinkSerialExecutionQueue.spotTimer(
                                    Runnable::run, spot);
                    var spotExecution = f.runtime.execution(() -> true, () -> discard(spot));
                    var actorExecution = f.runtime.execution(() -> true, () -> discard(actor));
                    spotExecution.ready().toCompletableFuture().join();
                    actorExecution.ready().toCompletableFuture().join();
                    spot.executionPolicy(spotExecution);
                    actor.executionPolicy(
                            application -> {
                                spotExecution.check(application);
                                actorExecution.check(application);
                            });
                    long reads = f.clockReads.get();
                    for (int turn = 0; turn < 100; turn++) {
                        for (var queue : java.util.List.of(spot, actor, timer)) {
                            queue.enqueue(() -> CompletableFuture.completedFuture(null), null)
                                    .toCompletableFuture()
                                    .join();
                        }
                    }
                    assertEquals(
                            reads,
                            f.clockReads.get(),
                            "ordinary Spot, member Actor and timer turns");
                    var creating =
                            new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                                    Runnable::run,
                                    systems.zlink.framework.execution.ZLinkExecutionLanePolicy
                                            .spot());
                    bind(f.runtime, creating, () -> false, () -> discard(creating));
                    f.failRenewal();
                    creating.enqueue(() -> CompletableFuture.completedFuture(null), null)
                            .toCompletableFuture()
                            .join();
                    f.now.set(100L + Duration.ofSeconds(25).toNanos());
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    creating.enqueue(
                                                    () -> CompletableFuture.completedFuture(null),
                                                    null)
                                            .toCompletableFuture()
                                            .join());
                }
            }
        }
    }

    @RepeatedTest(100)
    void executionGateContract() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean retention : new boolean[] {false, true}) {
                retainedGate(provider, retention);
                lateCompletion(provider, retention);
                ineligibleActivations(provider, retention);
                staleLeaseRecovery(provider, retention);
                retainedRecovery(provider, retention);
            }
        }
    }

    private void retainedGate(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            var queue =
                    new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                            Runnable::run,
                            systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
            var timer =
                    systems.zlink.framework.execution.ZLinkSerialExecutionQueue.spotTimer(
                            Runnable::run, queue);
            var cleaned = new java.util.concurrent.atomic.AtomicInteger();
            bind(
                    f.runtime,
                    queue,
                    () -> true,
                    () -> {
                        cleaned.incrementAndGet();
                        discard(queue);
                    });
            f.now.incrementAndGet();
            f.failRenewal();
            long cutoff = f.runtime.ownerLeaseSnapshot().outageCutoffNanos();
            f.now.set(cutoff - 1);
            var calls = new java.util.concurrent.atomic.AtomicInteger();
            queue.enqueue(
                            () -> {
                                calls.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            },
                            null)
                    .toCompletableFuture()
                    .join();
            timer.enqueue(
                            () -> {
                                calls.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            },
                            null)
                    .toCompletableFuture()
                    .join();
            assertEquals(2, calls.get());
            if (retention)
                assertThrows(IllegalStateException.class, f.runtime::ensureOwnerAdmissionOpen);
            var hold = new CompletableFuture<Void>();
            var started = new CompletableFuture<Void>();
            var active =
                    queue.enqueue(
                            () -> {
                                started.complete(null);
                                return hold;
                            },
                            null);
            started.join();
            var queued =
                    queue.enqueue(
                                    () -> {
                                        calls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null)
                            .toCompletableFuture();
            f.now.set(cutoff);
            f.fireCutoff();
            assertEquals(1, cleaned.get(), "T cleans the incarnation without another turn");
            assertTrue(queued.isDone(), "accepted queue entries are disposed at T");
            hold.complete(null);
            active.toCompletableFuture().join();
            assertEquals(2, calls.get());
            assertThrows(
                    CompletionException.class,
                    () ->
                            timer.enqueue(
                                            () -> {
                                                calls.incrementAndGet();
                                                return CompletableFuture.completedFuture(null);
                                            },
                                            null)
                                    .toCompletableFuture()
                                    .join());
            f.store.response = null;
            f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join();
            assertThrows(
                    RuntimeException.class,
                    () ->
                            queue.enqueue(
                                            () -> {
                                                calls.incrementAndGet();
                                                return CompletableFuture.completedFuture(null);
                                            },
                                            null)
                                    .toCompletableFuture()
                                    .join());
            assertEquals(2, calls.get(), "Store recovery cannot reopen a discarded incarnation");
            f.store.response = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
            f.store.claimResponse =
                    CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseClaimed(
                                    new ZLinkLocationOwnerToken("outage-owner", 2),
                                    Instant.EPOCH.plus(TTL),
                                    Instant.EPOCH));
            assertFalse(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertThrows(
                    RuntimeException.class,
                    () ->
                            queue.enqueue(
                                            () -> {
                                                calls.incrementAndGet();
                                                return CompletableFuture.completedFuture(null);
                                            },
                                            null)
                                    .toCompletableFuture()
                                    .join());
            assertEquals(1, cleaned.get());
            assertEquals(2, calls.get());
            assertThrows(
                    RuntimeException.class,
                    () ->
                            queue.enqueueLifecycleBarrier(
                                            () -> {
                                                calls.incrementAndGet();
                                                return CompletableFuture.completedFuture(null);
                                            })
                                    .toCompletableFuture()
                                    .join());
            assertEquals(2, calls.get());
        }
    }

    private void lateCompletion(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            var queue =
                    new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                            Runnable::run,
                            systems.zlink.framework.execution.ZLinkExecutionLanePolicy
                                    .spotReturningGate());
            bind(f.runtime, queue, () -> true, () -> discard(queue));
            var remote = new CompletableFuture<Void>();
            var started = new CompletableFuture<Void>();
            var resumed = new java.util.concurrent.atomic.AtomicInteger();
            var result =
                    queue.enqueue(
                            () -> {
                                started.complete(null);
                                return systems.zlink.framework.execution.ZLinkSerialExecutionQueue
                                        .manageCurrentInTurn(remote)
                                        .thenRun(resumed::incrementAndGet);
                            },
                            null);
            started.join();
            f.failRenewal();
            f.now.set(f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            f.fireCutoff();
            remote.complete(null);
            assertThrows(CompletionException.class, () -> result.toCompletableFuture().join());
            assertEquals(0, resumed.get(), "late completion cannot enter a disposed handler turn");
        }
    }

    private void ineligibleActivations(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            var creating =
                    new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                            Runnable::run,
                            systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
            var relocating =
                    new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                            Runnable::run,
                            systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
            bind(f.runtime, relocating, () -> false, () -> discard(relocating));
            f.failRenewal();
            bind(f.runtime, creating, () -> true, () -> discard(creating));
            f.now.set(100L + Duration.ofSeconds(25).toNanos());
            for (var queue : java.util.List.of(creating, relocating)) {
                assertThrows(
                        CompletionException.class,
                        () ->
                                queue.enqueue(() -> CompletableFuture.completedFuture(null), null)
                                        .toCompletableFuture()
                                        .join());
            }
        }
    }

    private void retainedRecovery(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            var queue =
                    new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                            Runnable::run,
                            systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
            var cleaned = new java.util.concurrent.atomic.AtomicInteger();
            bind(f.runtime, queue, () -> true, cleaned::incrementAndGet);
            var entered = new CompletableFuture<Void>();
            var release = new CompletableFuture<Void>();
            var active =
                    queue.enqueue(
                            () -> {
                                entered.complete(null);
                                return release;
                            },
                            null);
            entered.join();
            var calls = new java.util.concurrent.atomic.AtomicInteger();
            var pending =
                    queue.enqueue(
                            () -> {
                                calls.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            },
                            null);
            f.failRenewal();
            long cutoff = f.runtime.ownerLeaseSnapshot().outageCutoffNanos();
            f.now.set(cutoff - 1);
            f.store.response = null;
            assertTrue(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertNull(f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            release.complete(null);
            active.toCompletableFuture().join();
            pending.toCompletableFuture().join();
            assertEquals(1, calls.get());
            assertEquals(0, cleaned.get());
            queue.close();
        }
    }

    private void staleLeaseRecovery(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            var queue =
                    new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                            Runnable::run,
                            systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
            var cleanup = new java.util.concurrent.atomic.AtomicInteger();
            bind(
                    f.runtime,
                    queue,
                    () -> true,
                    () -> {
                        cleanup.incrementAndGet();
                        discard(queue);
                    });
            f.failRenewal();
            long cutoff = f.runtime.ownerLeaseSnapshot().outageCutoffNanos();
            f.store.response = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
            f.store.claimResponse =
                    CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseClaimed(
                                    new ZLinkLocationOwnerToken("outage-owner", 2),
                                    Instant.EPOCH.plus(TTL),
                                    Instant.EPOCH));
            assertFalse(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertEquals(
                    0, cleanup.get(), "stale lease alone does not prove object ownership loss");
            assertEquals(cutoff, f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            queue.enqueue(() -> CompletableFuture.completedFuture(null), null)
                    .toCompletableFuture()
                    .join();
            f.now.set(cutoff);
            f.fireCutoff();
            assertEquals(1, cleanup.get());
            assertThrows(
                    CompletionException.class,
                    () ->
                            queue.enqueue(() -> CompletableFuture.completedFuture(null), null)
                                    .toCompletableFuture()
                                    .join());
        }
    }

    private static void bind(
            ZLinkLocationRuntime runtime,
            systems.zlink.framework.execution.ZLinkSerialExecutionQueue queue,
            java.util.function.BooleanSupplier steady,
            Runnable cleanup) {
        var execution = runtime.execution(steady, cleanup);
        execution.ready().toCompletableFuture().join();
        queue.executionPolicy(execution);
    }

    private static void discard(systems.zlink.framework.execution.ZLinkSerialExecutionQueue queue) {
        queue.discardApplications().toCompletableFuture().join();
    }

    public static final class Fixture implements AutoCloseable {
        public final AtomicLong now = new AtomicLong(100);
        final AtomicLong clockReads = new AtomicLong();
        final ZLinkLocationFirstHeartbeatTest.RecordingScheduler scheduler =
                new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        final LeaseStore store;
        public final ZLinkLocationRuntime runtime;

        public Fixture(boolean provider, boolean retention) {
            var clock = Clock.fixed(Instant.EPOCH, ZoneOffset.UTC);
            ZLinkLocationRepository backing =
                    provider
                            ? new ZLinkProviderLocationRepository(
                                    new ZLinkInMemoryProviderLocationStore(clock))
                            : new ZLinkInMemoryLocationStore(clock);
            store = new LeaseStore(backing);
            runtime =
                    new ZLinkLocationRuntime(
                            ZLinkRegisteredLocationStores.fromUnified(store),
                            "outage-owner",
                            TTL,
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(3),
                            MARGIN,
                            () -> {
                                clockReads.incrementAndGet();
                                return now.get();
                            },
                            scheduler,
                            new ZLinkStateLane(Runnable::run));
            if (retention) runtime.setStoreOutageRetention(true);
            runtime.start(RoutingId.from("node")).toCompletableFuture().join();
        }

        public void failRenewal() {
            store.response =
                    CompletableFuture.failedFuture(
                            new IllegalStateException("provider unavailable"));
            assertFalse(runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
        }

        public void fireCutoff() {
            for (var task : java.util.List.copyOf(scheduler.tasks)) {
                if (!task.isCancelled()
                        && task.getDelay(TimeUnit.NANOSECONDS) > Duration.ofSeconds(5).toNanos())
                    task.run();
            }
        }

        public void close() {
            runtime.close();
        }
    }

    private static final class LeaseStore extends ZLinkLocationStoreTestAdapter {
        final ZLinkLocationRepository inner;
        CompletionStage<ZLinkOwnerLeaseRenewResult> response;
        CompletionStage<ZLinkOwnerLeaseReadResult> readResponse;
        CompletionStage<ZLinkOwnerLeaseClaimResult> claimResponse;
        boolean synchronousReadFailure;
        boolean synchronousRenewFailure;

        LeaseStore(ZLinkLocationRepository inner) {
            this.inner = inner;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String owner, Duration ttl) {
            return claimResponse == null ? inner.claimOwnerLease(owner, ttl) : claimResponse;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String owner) {
            if (synchronousReadFailure)
                throw new IllegalStateException("provider read failed synchronously");
            return readResponse == null ? inner.readOwnerLease(owner) : readResponse;
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                ZLinkLocationOwnerToken token, Duration ttl) {
            if (synchronousRenewFailure)
                throw new IllegalStateException("provider renew failed synchronously");
            return response == null ? inner.renewOwnerLease(token, ttl) : response;
        }

        @Override
        public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken token) {
            return inner.removeAllByOwner(token);
        }

        @Override
        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken token) {
            return inner.releaseOwnerLease(token);
        }
    }
}
