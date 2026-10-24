package systems.zlink.framework.runtime.internal.calls;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.time.Duration;
import java.util.function.LongSupplier;

/** Preserves one request budget across logical readiness and binding admission. */
public final class ZLinkRequestCalls {
    private ZLinkRequestCalls() {}

    public static long startTime(
            Duration timeoutOverride, Duration defaultTimeout, LongSupplier nanoTime) {
        return timeoutOverride == null && defaultTimeout == null ? 0 : nanoTime.getAsLong();
    }

    public static Duration remainingTimeout(Duration timeout, long started, LongSupplier nanoTime) {
        if (timeout == null) return null;
        long remaining = timeout.toNanos() - (nanoTime.getAsLong() - started);
        if (remaining <= 0) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    "request timed out before admission");
        }
        return Duration.ofNanos(remaining);
    }
}
