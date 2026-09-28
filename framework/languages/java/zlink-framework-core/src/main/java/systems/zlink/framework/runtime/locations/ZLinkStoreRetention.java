package systems.zlink.framework.runtime.locations;

import java.time.Duration;
import java.util.Objects;

/** Millisecond retention used by Location and Relocation Store providers. */
public final class ZLinkStoreRetention {
    private ZLinkStoreRetention() {}

    public static long toMillis(Duration retention) {
        Objects.requireNonNull(retention, "retention");
        if (retention.isZero() || retention.isNegative()) {
            throw new IllegalArgumentException("retention must be positive");
        }
        try {
            return Math.addExact(
                    Math.multiplyExact(retention.getSeconds(), 1000L),
                    (retention.getNano() + 999_999L) / 1_000_000L);
        } catch (ArithmeticException error) {
            throw new IllegalArgumentException("retention is too large", error);
        }
    }
}
