package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Delayed;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkLocationFirstHeartbeatTest {
    @Test
    void firstHeartbeatIsDueFromInitialClaimStart() throws Exception {
        assertFirstHeartbeat(false, false);
    }

    @Test
    void firstHeartbeatAfterClaimFailureIsDueFromInitialClaimStart() throws Exception {
        assertFirstHeartbeat(true, false);
    }

    @Test
    void renewalIntervalStartsAtStoreCallAfterMetrics() throws Exception {
        assertFirstHeartbeat(false, true);
    }

    private void assertFirstHeartbeat(boolean failClaim, boolean delayMetrics) throws Exception {
        var clock = new AtomicLong(Duration.ofSeconds(1).toNanos());
        var scheduler = new RecordingScheduler();
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        clock.addAndGet(Duration.ofMillis(50).toNanos());
                        if (failClaim)
                            return CompletableFuture.failedFuture(
                                    new IllegalStateException("Store unavailable"));
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseClaimed(
                                        new ZLinkLocationOwnerToken(ownerId, 1),
                                        Instant.EPOCH.plus(ttl),
                                        Instant.EPOCH));
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plus(ttl), Instant.EPOCH));
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(
                            String ownerId) {
                        return CompletableFuture.failedFuture(
                                new IllegalStateException("Store unavailable"));
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
        try (var metrics =
                        ZLinkRuntimeMetrics.install(
                                new ZLinkRuntimeMetrics.Sink() {
                                    @Override
                                    public void record(
                                            String name,
                                            Duration duration,
                                            java.util.Map<String, String> tags) {
                                        if (delayMetrics
                                                && name.equals(
                                                        "zlink.location.owner_lease.renew.lateness")) {
                                            clock.addAndGet(Duration.ofMillis(50).toNanos());
                                        }
                                    }
                                });
                var runtime =
                        new ZLinkLocationRuntime(
                                ZLinkRegisteredLocationStores.fromUnified(store),
                                "first-heartbeat",
                                Duration.ofSeconds(15),
                                Duration.ofMillis(100),
                                Duration.ofSeconds(1),
                                Duration.ofSeconds(5),
                                clock::get,
                                scheduler)) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            var heartbeat = scheduler.tasks.stream().filter(task -> !task.isCancelled()).toList();
            assertEquals(1, heartbeat.size());
            assertEquals(50, heartbeat.getFirst().getDelay(TimeUnit.MILLISECONDS));
            assertEquals(!failClaim, runtime.admittedOwnerToken().isPresent());
            if (delayMetrics) {
                clock.addAndGet(Duration.ofMillis(50).toNanos());
                var scheduled = scheduler.nextTask();
                heartbeat.getFirst().run();
                var next = scheduled.join();
                assertEquals(1, scheduler.tasks.stream().filter(task -> !task.isDone()).count());
                assertEquals(100, next.getDelay(TimeUnit.MILLISECONDS));
                clock.addAndGet(Duration.ofSeconds(10).toNanos());
            } else {
                clock.set(Duration.ofSeconds(11).toNanos());
            }
            assertEquals(false, runtime.admittedOwnerToken().isPresent());
        }
    }

    static final class RecordingScheduler extends ScheduledThreadPoolExecutor {
        final List<ScheduledTask<?>> tasks = new ArrayList<>();
        private volatile CompletableFuture<ScheduledTask<?>> nextScheduled;

        CompletableFuture<ScheduledTask<?>> nextTask() {
            var next = new CompletableFuture<ScheduledTask<?>>();
            nextScheduled = next;
            return next;
        }

        private <V> ScheduledFuture<V> record(ScheduledTask<V> task) {
            tasks.add(task);
            var next = nextScheduled;
            if (next != null) next.complete(task);
            return task;
        }

        RecordingScheduler() {
            super(1);
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            return record(new ScheduledTask<Void>(command, unit.toNanos(delay)));
        }

        @Override
        public <V> ScheduledFuture<V> schedule(Callable<V> command, long delay, TimeUnit unit) {
            return record(new ScheduledTask<V>(command, unit.toNanos(delay)));
        }
    }

    static final class ScheduledTask<V> extends FutureTask<V> implements ScheduledFuture<V> {
        private final long delayNanos;

        ScheduledTask(Runnable command, long delayNanos) {
            super(command, null);
            this.delayNanos = delayNanos;
        }

        ScheduledTask(Callable<V> command, long delayNanos) {
            super(command);
            this.delayNanos = delayNanos;
        }

        @Override
        public long getDelay(TimeUnit unit) {
            return unit.convert(delayNanos, TimeUnit.NANOSECONDS);
        }

        @Override
        public int compareTo(Delayed other) {
            return Long.compare(delayNanos, other.getDelay(TimeUnit.NANOSECONDS));
        }
    }
}
