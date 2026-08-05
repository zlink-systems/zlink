package systems.zlink.e2e.kotlin.discoveryregistryha.consumer;

import java.time.Duration;

public final class LocationStoreDelayState {
    private volatile int delayMilliseconds;

    public int delayMilliseconds() {
        return delayMilliseconds;
    }

    public void setDelay(Duration delay) {
        long clamped = Math.max(0, Math.min(5000, delay.toMillis()));
        delayMilliseconds = (int) clamped;
    }
}
