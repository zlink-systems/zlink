package systems.zlink.framework.spots;

import java.time.Duration;
import java.time.Instant;

public record ZLinkTimerTick(
    String name,
    long deliveryIndex,
    long scheduledIndex,
    Duration period,
    Instant scheduledAt,
    Instant startedAt,
    Duration scheduledElapsed,
    Duration startedElapsed,
    Duration delay,
    long skippedTicks) {
}
