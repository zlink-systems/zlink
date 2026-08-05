package systems.zlink.samples.gamequest.server.configuration;

import java.time.Duration;

public final class SampleTimings {
    public static final Duration RequestTimeout = Duration.ofSeconds(20);
    public static final Duration ConnectTimeout = Duration.ofSeconds(5);

    private SampleTimings() {
    }
}
