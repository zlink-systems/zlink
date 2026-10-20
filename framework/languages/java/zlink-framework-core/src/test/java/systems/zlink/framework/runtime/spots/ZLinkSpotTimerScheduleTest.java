package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;

import java.time.Duration;
import java.time.Instant;

final class ZLinkSpotTimerScheduleTest {
    @Test
    void preparingAnUndeliveredTickDoesNotConsumeDeliveryCounters() {
        var schedule =
                new ZLinkSpotTimerSchedule(
                        "timer",
                        Duration.ofSeconds(1),
                        timerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1));
        var pending = schedule.nextTick(Duration.ofSeconds(1).toNanos(), Instant.now());
        assertEquals(0, schedule.snapshot().deliveryIndex());
        assertEquals(0, schedule.snapshot().lastScheduledIndex());
        var repeated = schedule.nextTick(Duration.ofSeconds(1).toNanos(), Instant.now());
        assertEquals(pending.tick().deliveryIndex(), repeated.tick().deliveryIndex());
        schedule.markDelivered(pending);
        assertEquals(1, schedule.snapshot().deliveryIndex());
        assertEquals(1, schedule.snapshot().lastScheduledIndex());
        var next = schedule.nextTick(Duration.ofSeconds(2).toNanos(), Instant.now());
        assertEquals(2, next.tick().deliveryIndex());
        assertEquals(2, next.tick().scheduledIndex());
    }

    @Test
    void delayNextTickKeepsOneDeliveryPerDispatch() {
        ZLinkSpotTimerSchedule schedule =
                new ZLinkSpotTimerSchedule(
                        "timer",
                        Duration.ofMillis(10),
                        timerOptions(ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK, 1));

        ZLinkSpotTimerSchedule.PendingTick first =
                schedule.nextTick(Duration.ofMillis(100).toNanos(), Instant.now());
        schedule.markDelivered(first);
        ZLinkSpotTimerSchedule.PendingTick second =
                schedule.nextTick(Duration.ofMillis(200).toNanos(), Instant.now());

        assertEquals(1, first.tick().scheduledIndex());
        assertEquals(2, second.tick().scheduledIndex());
        assertEquals(0, second.tick().skippedTicks());
        assertEquals(Duration.ofMillis(10).toNanos(), schedule.delayAfterDispatchNanos());
    }

    @Test
    void skipLateTicksJumpsToCurrentDueIndex() {
        ZLinkSpotTimerSchedule schedule =
                new ZLinkSpotTimerSchedule(
                        "timer",
                        Duration.ofMillis(10),
                        timerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1));

        ZLinkSpotTimerSchedule.PendingTick tick =
                schedule.nextTick(Duration.ofMillis(55).toNanos(), Instant.now());

        assertEquals(5, tick.tick().scheduledIndex());
        assertEquals(4, tick.tick().skippedTicks());
    }

    @Test
    void boundedCatchUpStartsWithinCatchUpWindow() {
        ZLinkSpotTimerSchedule schedule =
                new ZLinkSpotTimerSchedule(
                        "timer",
                        Duration.ofMillis(10),
                        timerOptions(ZLinkTimerOverrunPolicy.CATCH_UP_BOUNDED, 3));

        ZLinkSpotTimerSchedule.PendingTick tick =
                schedule.nextTick(Duration.ofMillis(80).toNanos(), Instant.now());

        assertEquals(6, tick.tick().scheduledIndex());
        assertEquals(5, tick.tick().skippedTicks());
        assertTrue(tick.tick().scheduledElapsed().compareTo(Duration.ofMillis(60)) >= 0);
    }

    private static ZLinkTimerOptions timerOptions(
            ZLinkTimerOverrunPolicy overrunPolicy, int maxCatchUpTicks) {
        return new ZLinkTimerOptions(overrunPolicy, maxCatchUpTicks, false);
    }
}
