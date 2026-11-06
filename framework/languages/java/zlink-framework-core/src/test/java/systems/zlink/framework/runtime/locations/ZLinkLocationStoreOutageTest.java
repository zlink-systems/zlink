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

final class ZLinkLocationStoreOutageTest {
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
            // Reclaim a new lease before the old renewal response arrives.
            f.store.response = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
            f.store.claimResponse =
                    CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseClaimed(
                                    new ZLinkLocationOwnerToken("outage-owner", 2),
                                    Instant.EPOCH.plus(TTL),
                                    Instant.EPOCH));
            assertTrue(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertEquals(2, f.runtime.ownerLeaseSnapshot().token().leaseGeneration());
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
                    !retention, f.runtime.isOwnerAdmissionOpen(), "the latch is observation-only");
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

    private static final class Fixture implements AutoCloseable {
        final AtomicLong now = new AtomicLong(100);
        final ZLinkLocationFirstHeartbeatTest.RecordingScheduler scheduler =
                new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        final LeaseStore store;
        final ZLinkLocationRuntime runtime;

        Fixture(boolean provider, boolean retention) {
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
                            now::get,
                            scheduler,
                            new ZLinkStateLane(Runnable::run));
            if (retention) runtime.setStoreOutageRetention(true);
            runtime.start(RoutingId.from("node")).toCompletableFuture().join();
        }

        void failRenewal() {
            store.response =
                    CompletableFuture.failedFuture(
                            new IllegalStateException("provider unavailable"));
            assertFalse(runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
        }

        void fireCutoff() {
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
