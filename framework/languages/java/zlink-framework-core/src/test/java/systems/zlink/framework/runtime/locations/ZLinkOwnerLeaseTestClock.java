package systems.zlink.framework.runtime.locations;

import java.time.Duration;
import java.util.concurrent.atomic.AtomicLong;

/** Supplies a clock that advances a local lease without waiting for its scheduled callback. */
public final class ZLinkOwnerLeaseTestClock {
    public static final Duration TTL = Duration.ofMinutes(5);
    private final AtomicLong now = new AtomicLong(1);

    public ZLinkLocationRuntime runtime(ZLinkRegisteredLocationStores stores) {
        return new ZLinkLocationRuntime(
                stores, TTL, Duration.ofMinutes(1), Duration.ofSeconds(1), Duration.ZERO, now::get);
    }

    public void expire() {
        now.addAndGet(TTL.toNanos());
    }
}
