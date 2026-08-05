package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.time.Instant;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;

final class ZLinkSpotTimerSchedule {
    private final String name;
    private final Duration period;
    private final ZLinkTimerOptions options;
    private final Instant startedAt;
    private final long startedNanos;
    private final long periodNanos;
    private long deliveryIndex;
    private long lastScheduledIndex;

    ZLinkSpotTimerSchedule(String name, Duration period, ZLinkTimerOptions options) {
        this(
            name,
            period,
            options,
            Instant.now(),
            0L,
            0L);
    }

    ZLinkSpotTimerSchedule(State state) {
        this(
            state.name(),
            state.period(),
            state.options(),
            state.startedAt(),
            state.deliveryIndex(),
            state.lastScheduledIndex());
    }

    private ZLinkSpotTimerSchedule(
        String name,
        Duration period,
        ZLinkTimerOptions options,
        Instant startedAt,
        long deliveryIndex,
        long lastScheduledIndex) {
        this.name = name;
        this.period = period;
        this.options = options;
        this.startedAt = startedAt;
        long restoredElapsed;
        try {
            restoredElapsed = Math.max(
                0L,
                Duration.between(startedAt, Instant.now()).toNanos());
        } catch (ArithmeticException error) {
            restoredElapsed = Long.MAX_VALUE;
        }
        this.startedNanos = System.nanoTime() - restoredElapsed;
        this.periodNanos = Math.max(1L, period.toNanos());
        this.deliveryIndex = deliveryIndex;
        this.lastScheduledIndex = lastScheduledIndex;
    }

    long initialDelayNanos() {
        return periodNanos;
    }

    long startedElapsedNanos() {
        return Math.max(0L, System.nanoTime() - startedNanos);
    }

    PendingTick nextTick(long startedElapsedNanos, Instant now) {
        long scheduledIndex = nextScheduledIndex(startedElapsedNanos);
        long skipped = options.overrunPolicy() == ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK
            ? 0L
            : scheduledIndex - lastScheduledIndex - 1L;
        long index = ++deliveryIndex;
        long scheduledElapsedNanos = saturatedMultiply(periodNanos, scheduledIndex);
        ZLinkTimerTick tick = new ZLinkTimerTick(
            name,
            index,
            scheduledIndex,
            period,
            startedAt.plusNanos(scheduledElapsedNanos),
            now,
            Duration.ofNanos(scheduledElapsedNanos),
            Duration.ofNanos(startedElapsedNanos),
            Duration.between(startedAt.plusNanos(scheduledElapsedNanos), now),
            skipped);
        return new PendingTick(scheduledIndex, tick);
    }

    void markDelivered(PendingTick pendingTick) {
        lastScheduledIndex = pendingTick.scheduledIndex();
    }

    State snapshot() {
        return new State(
            name,
            period,
            options,
            startedAt,
            deliveryIndex,
            lastScheduledIndex);
    }

    long delayAfterDispatchNanos() {
        if (options.overrunPolicy() == ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK) {
            return periodNanos;
        }
        long nextScheduledElapsed = saturatedMultiply(periodNanos, lastScheduledIndex + 1L);
        return nextScheduledElapsed - startedElapsedNanos();
    }

    private long nextScheduledIndex(long startedElapsedNanos) {
        if (options.overrunPolicy() == ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK) {
            return deliveryIndex + 1L;
        }
        long next = lastScheduledIndex + 1L;
        long due = Math.max(next, Math.max(1L, startedElapsedNanos / periodNanos));
        if (options.overrunPolicy() == ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS) {
            return due;
        }
        long available = due - lastScheduledIndex;
        long maxCatchUp = options.maxCatchUpTicks();
        if (available > maxCatchUp) {
            return due - maxCatchUp + 1L;
        }
        return next;
    }

    private static long saturatedMultiply(long left, long right) {
        if (left <= 0 || right <= 0) {
            return 0L;
        }
        if (left > Long.MAX_VALUE / right) {
            return Long.MAX_VALUE;
        }
        return left * right;
    }

    record PendingTick(long scheduledIndex, ZLinkTimerTick tick) {
    }

    record State(
        String name,
        Duration period,
        ZLinkTimerOptions options,
        Instant startedAt,
        long deliveryIndex,
        long lastScheduledIndex) {
    }
}
