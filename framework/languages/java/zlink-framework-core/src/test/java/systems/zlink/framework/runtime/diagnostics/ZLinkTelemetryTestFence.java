package systems.zlink.framework.runtime.diagnostics;

import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.LockSupport;

/** Test-only bounded observation; production has no flush operation or test wake notifications. */
public final class ZLinkTelemetryTestFence {
    private static final long WAIT_SECONDS = 10;
    private static final long POLL_NANOS = TimeUnit.MICROSECONDS.toNanos(100);

    private ZLinkTelemetryTestFence() {}

    public static void awaitIdle() {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(WAIT_SECONDS);
        while (true) {
            var state = ZLinkTelemetryOwner.state();
            if (!state.executing() && state.pending() == 0) return;
            long remaining = end - System.nanoTime();
            if (remaining <= 0) throw new AssertionError("telemetry did not drain");
            if (Thread.currentThread().isInterrupted())
                throw new AssertionError("interrupted waiting for telemetry");
            LockSupport.parkNanos(Math.min(POLL_NANOS, remaining));
        }
    }
}
