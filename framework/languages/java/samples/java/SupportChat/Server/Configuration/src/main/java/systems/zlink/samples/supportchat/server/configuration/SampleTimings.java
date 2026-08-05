package systems.zlink.samples.supportchat.server.configuration;

import java.time.Duration;

public final class SampleTimings {
    public static final Duration ConnectTimeout = Duration.ofSeconds(5);
    public static final Duration RequestTimeout = Duration.ofSeconds(5);
    public static final Duration IdleTimeout = Duration.ofSeconds(3);
    public static final Duration CloseGraceTimeout = Duration.ofSeconds(2);

    private SampleTimings() {
    }
}
