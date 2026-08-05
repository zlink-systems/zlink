package systems.zlink.samples.bingo.client.configuration;

import java.time.Duration;

public final class SampleTimings {
    public static final Duration ConnectTimeout = Duration.ofSeconds(5);
    public static final Duration RequestTimeout = Duration.ofSeconds(30);

    private SampleTimings() {
    }
}
