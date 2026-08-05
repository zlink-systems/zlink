/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.internal;

import java.time.Duration;

public final class DurationConversions {
    private DurationConversions() {
    }

    public static int toIntMillis(Duration timeout, String name) {
        if (timeout == null) {
            throw new NullPointerException(name);
        }
        long millis = timeout.toMillis();
        if (millis < Integer.MIN_VALUE || millis > Integer.MAX_VALUE) {
            throw new IllegalArgumentException(
                name + " millis out of int range: " + millis);
        }
        return (int) millis;
    }

    public static int timeoutMillisOrZero(Duration timeout) {
        if (timeout == null || timeout.isZero()) {
            return 0;
        }
        long millis = Math.max(1L, timeout.toMillis());
        return millis >= Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) millis;
    }

    public static long timeoutMillisOrDefault(Duration timeout,
                                              long defaultMillis) {
        if (timeout == null) {
            return defaultMillis;
        }
        return Math.max(1L, timeout.toMillis());
    }
}
