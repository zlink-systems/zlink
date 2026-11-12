package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkOwnerLeaseOutageRecoveryTest {
    private static final Duration TTL = Duration.ofSeconds(30);
    private static final Duration MARGIN = Duration.ofSeconds(5);
    private static final long RECLAIM_AFTER = TTL.plus(MARGIN).toNanos();

    static final class MutableClock extends Clock {
        Instant now = Instant.EPOCH;

        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        public Clock withZone(ZoneId zone) {
            return this;
        }

        public Instant instant() {
            return now;
        }
    }

    static final class LeaseStore extends ZLinkLocationStoreTestAdapter {
        final ZLinkLocationRepository backing;
        CompletionStage<ZLinkOwnerLeaseRenewResult> renew;
        CompletionStage<ZLinkOwnerLeaseReadResult> exact;
        CompletionStage<ZLinkOwnerLeaseClaimResult> claim;
        Runnable onClaim;
        int claims;
        int reads;

        LeaseStore(ZLinkLocationRepository backing) {
            this.backing = backing;
        }

        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String id, Duration ttl) {
            claims++;
            if (onClaim != null) onClaim.run();
            return claim == null ? backing.claimOwnerLease(id, ttl) : claim;
        }

        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                ZLinkLocationOwnerToken token, Duration ttl) {
            return renew == null ? backing.renewOwnerLease(token, ttl) : renew;
        }

        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String id) {
            reads++;
            return exact == null ? backing.readOwnerLease(id) : exact;
        }

        public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken token) {
            return backing.removeAllByOwner(token);
        }

        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken token) {
            return backing.releaseOwnerLease(token);
        }
    }

    static final class Fixture implements AutoCloseable {
        final MutableClock clock = new MutableClock();
        final AtomicLong now = new AtomicLong();
        final AtomicLong clockReads = new AtomicLong();
        final ZLinkLocationFirstHeartbeatTest.RecordingScheduler scheduler =
                new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        final LeaseStore store;
        final ZLinkLocationRuntime runtime;
        final AtomicInteger disposed = new AtomicInteger();
        final AtomicInteger collected = new AtomicInteger();
        final ZLinkLocationRuntime.Execution execution;
        final ZLinkLocationOwnerToken original;

        Fixture(boolean provider, boolean retention) {
            store =
                    new LeaseStore(
                            provider
                                    ? new ZLinkProviderLocationRepository(
                                            new ZLinkInMemoryProviderLocationStore(clock))
                                    : new ZLinkInMemoryLocationStore(clock));
            runtime =
                    new ZLinkLocationRuntime(
                            ZLinkRegisteredLocationStores.fromUnified(store),
                            "owner",
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
            runtime.setStoreOutageRetention(retention);
            await(runtime.start(RoutingId.from("node")));
            original = runtime.currentOwnerToken();
            execution =
                    runtime.execution(
                            () -> {
                                collected.incrementAndGet();
                                return true;
                            },
                            disposed::incrementAndGet);
            await(execution.ready());
        }

        void at(long nanos) {
            now.set(nanos);
            clock.now = Instant.EPOCH.plusNanos(nanos);
        }

        void outage() {
            store.renew =
                    CompletableFuture.failedFuture(new IllegalStateException("Store unavailable"));
            assertFalse(await(runtime.renewOwnerLeaseOnce()));
            store.renew = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        long cutoff() {
            return runtime.ownerLeaseSnapshot().outageCutoffNanos();
        }

        public void close() {
            runtime.close();
        }
    }

    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void discardedEpisodeDoesNotBlockSecondOutage(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.outage();
            long firstCutoff = f.cutoff();
            f.at(firstCutoff);
            for (var task : java.util.List.copyOf(f.scheduler.tasks))
                if (!task.isCancelled()
                        && task.getDelay(TimeUnit.NANOSECONDS) > Duration.ofSeconds(5).toNanos())
                    task.run();
            assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            assertEquals(1, f.disposed.get());
            assertStale(CompletableFuture.runAsync(() -> f.execution.check(true), Runnable::run));

            f.at(Math.max(firstCutoff, RECLAIM_AFTER) + 1);
            assertTrue(
                    await(f.runtime.renewOwnerLeaseOnce()),
                    "provider="
                            + provider
                            + ", retention="
                            + retention
                            + ", snapshot="
                            + f.runtime.ownerLeaseSnapshot());
            assertNotEquals(f.original, f.runtime.currentOwnerToken());
            var disposed = new AtomicInteger();
            var second = f.runtime.execution(() -> true, disposed::incrementAndGet);
            await(second.ready());
            second.check(true);
            long basis = f.runtime.ownerLeaseSnapshot().lastSuccessfulRequestStartedNanos();
            f.at(f.now.get() + 1);
            f.outage();
            long secondCutoff = basis + Duration.ofSeconds(retention ? 40 : 25).toNanos();
            assertEquals(f.now.get(), f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            assertEquals(secondCutoff, f.cutoff());
            assertFalse(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            f.at(secondCutoff - 1);
            second.check(true);
            assertEquals(0, disposed.get());
            assertStale(CompletableFuture.runAsync(() -> f.execution.check(true), Runnable::run));
            f.at(secondCutoff);
            for (var task : java.util.List.copyOf(f.scheduler.tasks))
                if (!task.isCancelled()
                        && task.getDelay(TimeUnit.NANOSECONDS) > Duration.ofSeconds(5).toNanos())
                    task.run();
            assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            assertEquals(1, disposed.get());
            assertStale(CompletableFuture.runAsync(() -> second.check(true), Runnable::run));
            assertStale(CompletableFuture.runAsync(() -> f.execution.check(true), Runnable::run));
        }
    }

    private static <T> T await(CompletionStage<T> stage) {
        return stage.toCompletableFuture().join();
    }

    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void deadlineLatchHoldsAcceptedWorkUntilRenewal(boolean provider, boolean actor)
            throws Exception {
        try (var f = new Fixture(provider, true)) {
            var scheduled = new LinkedBlockingQueue<Runnable>();
            var queue =
                    new ZLinkSerialExecutionQueue(
                            scheduled::add,
                            actor
                                    ? ZLinkExecutionLanePolicy.actorDelivery()
                                    : ZLinkExecutionLanePolicy.spot());
            queue.executionPolicy(f.execution);
            var timer = ZLinkSerialExecutionQueue.spotTimer(scheduled::add, queue);
            f.execution.onResume(
                    () -> {
                        queue.resumePending();
                        timer.resumePending();
                    });
            var timerCalls = new AtomicInteger();
            var queuedTick =
                    timer.enqueue(
                                    () -> {
                                        timerCalls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null)
                            .toCompletableFuture();
            var calls = new AtomicInteger();
            var accepted =
                    queue.enqueue(
                                    () -> {
                                        calls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null)
                            .toCompletableFuture();
            f.at(Duration.ofSeconds(25).toNanos());
            for (var task : java.util.List.copyOf(f.scheduler.tasks))
                if (!task.isCancelled()
                        && task.getDelay(TimeUnit.NANOSECONDS) == Duration.ofSeconds(25).toNanos())
                    task.run();
            assertTrue(f.execution.paused(), "the deadline latch temporarily closes execution");
            while (!scheduled.isEmpty()) scheduled.remove().run();
            assertFalse(
                    accepted.isDone(), "accepted work waits while the deadline latch is closed");
            assertEquals(0, calls.get());
            assertFalse(queuedTick.isDone());
            assertEquals(0, timerCalls.get());
            var rejected =
                    assertThrows(
                            ZLinkFrameworkException.class, f.runtime::ensureOwnerAdmissionOpen);
            assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, rejected.kind());
            assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
            assertFalse(f.execution.paused(), "renewal reopens the execution gate");
            var completed = CompletableFuture.allOf(accepted, queuedTick);
            while (!completed.isDone()) {
                var task = scheduled.poll(2, TimeUnit.SECONDS);
                if (task == null) assertTrue(completed.isDone(), "renewal wakes accepted work");
                else task.run();
            }
            completed.get(2, TimeUnit.SECONDS);
            await(accepted);
            assertEquals(1, calls.get());
            assertTrue(queuedTick.isDone());
            await(queuedTick);
            assertEquals(1, timerCalls.get());
            timer.close();
            queue.close();
        }
    }

    @RepeatedTest(100)
    void staleDuringOutageNeedsLocalExpiryAndExactEvidence() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean retention : new boolean[] {false, true}) {
                try (var f = new Fixture(provider, retention)) {
                    f.outage();
                    long cutoff = f.cutoff();
                    assertFalse(
                            await(f.runtime.renewOwnerLeaseOnce()),
                            "live token cannot be replaced on stale alone");
                    assertEquals(1, f.store.claims, "only the startup claim");
                    assertEquals(f.original, f.runtime.currentOwnerToken());
                    assertEquals(0, f.disposed.get());
                    f.execution.check(true);
                    f.at(RECLAIM_AFTER - 1);
                    f.store.exact = CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
                    assertFalse(
                            await(f.runtime.renewOwnerLeaseOnce()),
                            "Missing alone cannot shorten the monotonic wait");
                    assertEquals(1, f.store.claims);
                    f.at(RECLAIM_AFTER);
                    f.store.exact =
                            CompletableFuture.completedFuture(
                                    new ZLinkOwnerLeaseFound(
                                            f.original, f.clock.now.plus(TTL), f.clock.now));
                    assertFalse(
                            await(f.runtime.renewOwnerLeaseOnce()),
                            "monotonic expiry alone cannot replace a live exact lease");
                    assertEquals(1, f.store.claims);
                    f.store.exact = null;
                    assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                    assertEquals(2, f.store.claims);
                    assertNotEquals(f.original, f.runtime.currentOwnerToken());
                    assertEquals(cutoff, f.cutoff(), "new claim preserves the original T");
                }
                try (var f = new Fixture(provider, retention)) {
                    f.outage();
                    f.at(RECLAIM_AFTER);
                    f.store.exact =
                            CompletableFuture.completedFuture(
                                    new ZLinkOwnerLeaseFound(f.original, f.clock.now, f.clock.now));
                    assertTrue(
                            await(f.runtime.renewOwnerLeaseOnce()),
                            "exact expiration at StoreNow also allows claim");
                    assertEquals(2, f.store.claims);
                }
            }
        }
    }

    @RepeatedTest(100)
    void definitiveForeignTokenDiscardsButReadFailureDoesNot() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.outage();
                long cutoff = f.cutoff();
                f.at(RECLAIM_AFTER);
                f.store.exact =
                        CompletableFuture.failedFuture(
                                new IllegalStateException("exact read unavailable"));
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(0, f.disposed.get());
                assertEquals(1, f.store.claims);
                f.store.exact =
                        CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseFound(
                                        new ZLinkLocationOwnerToken(
                                                "owner", f.original.leaseGeneration() + 1),
                                        f.clock.now.plus(TTL),
                                        f.clock.now));
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(1, f.disposed.get());
                assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
                assertEquals(cutoff, f.cutoff());
                assertEquals(
                        1, f.store.claims, "foreign token must not enter claim/conflict adoption");
            }
        }
    }

    @RepeatedTest(100)
    void claimConflictCannotAdoptAnotherIncarnationDuringEpisode() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(RECLAIM_AFTER);
                f.store.exact = CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
                f.store.claim =
                        CompletableFuture.completedFuture(new ZLinkOwnerLeaseClaimConflict());
                f.store.onClaim =
                        () ->
                                f.store.exact =
                                        CompletableFuture.completedFuture(
                                                new ZLinkOwnerLeaseFound(
                                                        new ZLinkLocationOwnerToken(
                                                                "owner",
                                                                f.original.leaseGeneration() + 1),
                                                        f.clock.now.plus(TTL),
                                                        f.clock.now));
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(f.original, f.runtime.ownerLeaseSnapshot().token());
                assertEquals(1, f.disposed.get());
                assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
                assertEquals(
                        2,
                        f.store.claims,
                        "one recovery attempt followed by a definitive conflict");
            }
        }
    }

    @RepeatedTest(100)
    void exactReadCannotOvertakeSuccessfulRenewalOrShutdown() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(RECLAIM_AFTER);
                var exact = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                f.store.exact = exact;
                var stale = f.runtime.renewOwnerLeaseOnce();
                assertFalse(stale.toCompletableFuture().isDone());
                // A state-lane diagnostic turn completes while Store IO is pending.
                assertEquals(f.original, f.runtime.ownerLeaseSnapshot().token());
                f.store.renew =
                        CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseRenewed(f.clock.now.plus(TTL), f.clock.now));
                assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                exact.complete(new ZLinkOwnerLeaseMissing());
                assertFalse(await(stale));
                assertEquals(1, f.store.claims);
                assertEquals(f.original, f.runtime.currentOwnerToken());
                assertEquals(0, f.disposed.get());
            }
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(RECLAIM_AFTER);
                var exact = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                f.store.exact = exact;
                var stale = f.runtime.renewOwnerLeaseOnce();
                f.runtime.close();
                exact.complete(new ZLinkOwnerLeaseMissing());
                assertFalse(await(stale));
                assertEquals(1, f.store.claims);
            }
        }
    }

    @RepeatedTest(100)
    void exactReadAndClaimShareTheExistingRenewTimeout() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(RECLAIM_AFTER);
                var exact = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                f.store.exact = exact;
                var stale = f.runtime.renewOwnerLeaseOnce();
                assertFalse(stale.toCompletableFuture().isDone());
                f.at(RECLAIM_AFTER + Duration.ofSeconds(3).toNanos());
                exact.complete(new ZLinkOwnerLeaseMissing());
                assertFalse(await(stale));
                assertEquals(
                        1,
                        f.store.claims,
                        "a completed read cannot allocate a fresh claim timeout");
                assertEquals(0, f.disposed.get());
            }
        }
    }

    @RepeatedTest(100)
    void latePreOutageResponseCannotConfirmRecoveryAndOutsideEpisodeStillClaims() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                var late = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
                f.store.renew = late;
                var pending = f.runtime.renewOwnerLeaseOnce();
                f.at(1);
                f.outage();
                long cutoff = f.cutoff();
                late.complete(new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plus(TTL), Instant.EPOCH));
                assertFalse(await(pending));
                assertEquals(cutoff, f.cutoff());
                assertEquals(0, f.disposed.get());
            }
            try (var f = new Fixture(provider, false)) {
                f.store.renew = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
                assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(2, f.store.claims, "stale outside an episode keeps immediate claim");
                assertNull(f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            }
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(1);
                f.store.renew = null;
                assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(1, f.store.claims, "live lease resumes with the same token");
                assertEquals(f.original, f.runtime.currentOwnerToken());
            }
        }
    }

    @RepeatedTest(100)
    void fiveThousandRetainedExecutionsHaveConstantChecksAndSingleCollection() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                for (int i = 1; i < 5000; i++)
                    await(
                            f.runtime
                                    .execution(
                                            () -> {
                                                f.collected.incrementAndGet();
                                                return true;
                                            },
                                            f.disposed::incrementAndGet)
                                    .ready());
                long reads = f.clockReads.get();
                for (int i = 0; i < 5000; i++) f.execution.check(true);
                assertEquals(reads, f.clockReads.get(), "ordinary execution uses no clock");
                assertEquals(0, f.collected.get());
                f.outage();
                assertEquals(5000, f.collected.get(), "one visit per collected execution");
                f.at(Duration.ofSeconds(25).toNanos());
                reads = f.clockReads.get();
                for (int i = 0; i < 5000; i++) f.execution.check(true);
                assertEquals(
                        reads + 10000,
                        f.clockReads.get(),
                        "two local deadline reads per retained check");
                assertEquals(
                        5000, f.collected.get(), "execution checks do not enumerate other owners");
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(
                        5000, f.collected.get(), "reclaim exclusion does not recollect owners");
                assertEquals(1, f.store.claims);
                assertEquals(
                        0, f.store.reads, "reclaim before local expiration submits no exact read");
            }
        }
    }

    private static void assertStale(CompletionStage<?> stage) {
        Throwable failure = assertThrows(CompletionException.class, () -> await(stage)).getCause();
        while (failure instanceof CompletionException && failure.getCause() != null)
            failure = failure.getCause();
        assertTrue(
                systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin.isFramework(
                        failure));
        assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                assertInstanceOf(ZLinkFrameworkException.class, failure).kind());
    }

    @RepeatedTest(100)
    void cutoffAndDefinitiveLossEndPendingAndResumedWorkWithStaleRoute() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean cutoff : new boolean[] {false, true}) {
                try (var f = new Fixture(provider, true)) {
                    var queue =
                            new ZLinkSerialExecutionQueue(
                                    Runnable::run, ZLinkExecutionLanePolicy.spot());
                    var timer = ZLinkSerialExecutionQueue.spotTimer(Runnable::run, queue);
                    var execution =
                            f.runtime.execution(
                                    () -> true,
                                    () -> {
                                        queue.discardApplications();
                                        timer.discardApplications();
                                    });
                    await(execution.ready());
                    queue.executionPolicy(execution);
                    var entered = new CompletableFuture<Void>();
                    var remote = new CompletableFuture<Void>();
                    var active =
                            queue.enqueue(
                                    () -> {
                                        entered.complete(null);
                                        return remote;
                                    },
                                    null);
                    entered.join();
                    var called = new AtomicInteger();
                    var pending =
                            queue.enqueue(
                                    () -> {
                                        called.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null);
                    f.outage();
                    if (cutoff) {
                        f.at(f.cutoff());
                        for (var task : java.util.List.copyOf(f.scheduler.tasks))
                            if (!task.isCancelled()
                                    && task.getDelay(TimeUnit.NANOSECONDS)
                                            > Duration.ofSeconds(5).toNanos()) task.run();
                    } else execution.discard();
                    assertStale(pending);
                    assertStale(
                            queue.enqueue(
                                    () -> {
                                        called.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null));
                    assertStale(
                            timer.enqueue(
                                    () -> {
                                        called.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null));
                    assertEquals(0, called.get());
                    remote.complete(null);
                    await(active);
                    queue.close();
                    timer.close();
                }
                try (var f = new Fixture(provider, true)) {
                    var queue =
                            new ZLinkSerialExecutionQueue(
                                    Runnable::run, ZLinkExecutionLanePolicy.spotReturningGate());
                    var execution =
                            f.runtime.execution(() -> true, () -> queue.discardApplications());
                    await(execution.ready());
                    queue.executionPolicy(execution);
                    var remote = new CompletableFuture<Void>();
                    var entered = new CompletableFuture<Void>();
                    var resumed = new AtomicInteger();
                    var pending =
                            queue.enqueue(
                                    () -> {
                                        entered.complete(null);
                                        return ZLinkSerialExecutionQueue.manageCurrentInTurn(remote)
                                                .thenRun(resumed::incrementAndGet);
                                    },
                                    null);
                    entered.join();
                    f.outage();
                    if (cutoff) {
                        f.at(f.cutoff());
                        for (var task : java.util.List.copyOf(f.scheduler.tasks))
                            if (!task.isCancelled()
                                    && task.getDelay(TimeUnit.NANOSECONDS)
                                            > Duration.ofSeconds(5).toNanos()) task.run();
                    } else execution.discard();
                    remote.complete(null);
                    assertStale(pending);
                    assertEquals(0, resumed.get());
                    queue.close();
                }
            }
        }
    }

    @RepeatedTest(100)
    void expiredTokenCannotBeRenewedByEitherRepository() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.at(TTL.toNanos() - 1);
                assertInstanceOf(
                        ZLinkOwnerLeaseRenewed.class,
                        await(f.store.backing.renewOwnerLease(f.original, TTL)));
                f.at(TTL.multipliedBy(2).toNanos() - 1);
                assertInstanceOf(
                        ZLinkOwnerLeaseRenewStale.class,
                        await(f.store.backing.renewOwnerLease(f.original, TTL)));
            }
        }
    }
}
