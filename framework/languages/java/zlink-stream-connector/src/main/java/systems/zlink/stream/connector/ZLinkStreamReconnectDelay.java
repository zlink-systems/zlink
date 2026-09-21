package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.concurrent.ThreadLocalRandom;
import java.util.function.DoubleSupplier;

/**
 * Reconnect delay arithmetic, kept as pure functions so a test can drive the randomness instead of
 * sampling it.
 *
 * <p>Common connector spec §6: the base delay starts at the initial delay, is multiplied by the
 * backoff factor on each attempt and stops at the maximum delay. The time actually waited is a
 * value picked between 50% and 100% of that base delay, so clients that were connected to one
 * server do not all come back at the same instant.
 */
final class ZLinkStreamReconnectDelay {
    /** Lower bound of the jitter window, as a fraction of the base delay. */
    static final double MIN_JITTER_FRACTION = 0.5;

    private ZLinkStreamReconnectDelay() {}

    /**
     * The base delay for the attempt after one that waited {@code current}. Deterministic: jitter
     * is applied by {@link #jittered} at wait time and never feeds back into the base, so the
     * growth curve stays predictable.
     */
    static Duration nextBase(
            Duration current, ZLinkStreamConnectorConfiguration.Reconnect reconnect) {
        long nextMillis = Math.round(current.toMillis() * reconnect.backoffFactor());
        if (nextMillis <= 0) {
            nextMillis = reconnect.initialDelay().toMillis();
        }
        return Duration.ofMillis(Math.min(nextMillis, reconnect.maxDelay().toMillis()));
    }

    /**
     * The time to wait for one attempt: a value in {@code [0.5 * base, base]}. {@code random}
     * yields values in {@code [0, 1)}.
     */
    static Duration jittered(Duration base, DoubleSupplier random) {
        long baseMillis = base.toMillis();
        if (baseMillis <= 0) {
            return Duration.ZERO;
        }
        double fraction =
                MIN_JITTER_FRACTION + (1.0 - MIN_JITTER_FRACTION) * clampUnit(random.getAsDouble());
        long millis = Math.round(baseMillis * fraction);
        //  Rounding must not push the wait outside the window at either end.
        long floor = (long) Math.ceil(baseMillis * MIN_JITTER_FRACTION);
        return Duration.ofMillis(Math.max(floor, Math.min(millis, baseMillis)));
    }

    static Duration jittered(Duration base) {
        return jittered(base, () -> ThreadLocalRandom.current().nextDouble());
    }

    private static double clampUnit(double value) {
        if (Double.isNaN(value) || value < 0.0) {
            return 0.0;
        }
        return Math.min(value, 1.0);
    }
}
