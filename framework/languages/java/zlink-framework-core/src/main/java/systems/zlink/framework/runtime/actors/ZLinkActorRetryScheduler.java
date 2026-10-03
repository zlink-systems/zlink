package systems.zlink.framework.runtime.actors;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

final class ZLinkActorRetryScheduler {
    private ZLinkActorRetryScheduler() {}

    /** Schedules the next Session route retransmission at the spec 20 §5 interval. */
    static void scheduleRouteAfter(Runnable attempt, Duration delay) {
        if (delay == null || delay.isNegative() || delay.isZero()) {
            CompletableFuture.runAsync(attempt);
            return;
        }
        scheduleAfter(attempt, delay);
    }

    static CompletableFuture<Void> scheduleAfter(Runnable attempt, Duration delay) {
        return CompletableFuture.runAsync(
                attempt,
                CompletableFuture.delayedExecutor(delay.toMillis(), TimeUnit.MILLISECONDS));
    }
}
