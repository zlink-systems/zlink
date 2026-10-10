package systems.zlink.framework.runtime.internal.channels;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.time.Duration;

/** Shared channel send-timeout default, validation, and millisecond normalization. */
public final class ZLinkChannelAdmissionTimeout {
    public static final Duration DEFAULT_SEND_TIMEOUT = Duration.ofSeconds(1);

    private ZLinkChannelAdmissionTimeout() {}

    public static Duration normalize(Duration timeout) {
        return Duration.ofMillis(normalizedMillis(timeout));
    }

    public static int normalizedMillis(Duration timeout) {
        if (timeout == null || timeout.isZero() || timeout.isNegative()) {
            throw new ZLinkConfigurationException("send timeout must be positive");
        }
        long seconds = timeout.getSeconds();
        if (seconds > Integer.MAX_VALUE / 1000L) {
            throw new ZLinkConfigurationException(
                    "send timeout must normalize to at most Integer.MAX_VALUE ms");
        }
        long millis = seconds * 1000L + (timeout.getNano() + 999_999L) / 1_000_000L;
        if (millis < 1L || millis > Integer.MAX_VALUE) {
            throw new ZLinkConfigurationException(
                    "send timeout must normalize to 1..Integer.MAX_VALUE ms");
        }
        return (int) millis;
    }
}
