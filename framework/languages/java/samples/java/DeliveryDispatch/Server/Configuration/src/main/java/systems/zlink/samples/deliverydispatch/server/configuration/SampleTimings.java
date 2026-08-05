package systems.zlink.samples.deliverydispatch.server.configuration;

import java.time.Duration;

public final class SampleTimings {
    public static final Duration RequestTimeout = Duration.ofSeconds(10);
    /**
     * How long an offer stands before the sweeper reassigns it. The deadline belongs to the
     * dispatch worker, not to the courier node (common sample spec section 7.4).
     */
    public static final Duration CourierDecisionTimeout = Duration.ofMillis(900);
    public static final Duration OfferSweepInterval = Duration.ofMillis(100);

    private SampleTimings() {
    }
}
