package systems.zlink.samples.shoppingmall.server.configuration;

import java.time.Duration;

public final class SampleTimings {
    public static final Duration HttpTimeout = Duration.ofSeconds(10);
    public static final Duration WorkflowTimeout = Duration.ofSeconds(30);
    public static final Duration RequestTimeout = Duration.ofSeconds(30);

    private SampleTimings() {
    }
}
