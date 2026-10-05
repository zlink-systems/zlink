package systems.zlink.framework.runtime.internal.calls;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.time.Duration;

/** Preserves one request budget across logical readiness and binding admission. */
public final class ZLinkRequestCalls {
    private ZLinkRequestCalls() {}

    public static Duration remainingTimeout(Duration timeout, long started, long now) {
        if (timeout == null) return null;
        long remaining = timeout.toNanos() - (now - started);
        if (remaining <= 0) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    "request timed out before admission");
        }
        return Duration.ofNanos(remaining);
    }
}
