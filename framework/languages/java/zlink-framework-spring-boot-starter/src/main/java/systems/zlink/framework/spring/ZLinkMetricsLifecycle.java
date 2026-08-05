package systems.zlink.framework.spring;

import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.Metrics;
import java.util.List;
import org.springframework.context.SmartLifecycle;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

final class ZLinkMetricsLifecycle implements SmartLifecycle {
    private final MeterRegistry registry;
    private AutoCloseable registration;
    private volatile boolean running;

    ZLinkMetricsLifecycle(MeterRegistry registry, List<ZLinkMetricsCustomizer> customizers) {
        this.registry = registry;
        for (ZLinkMetricsCustomizer customizer : customizers) {
            customizer.customize(registry);
        }
    }

    @Override
    public void start() {
        if (!running) {
            Metrics.addRegistry(registry);
            registration = ZLinkRuntimeMetrics.install(new ZLinkMicrometerMetricSink(registry));
            running = true;
        }
    }

    @Override
    public void stop() {
        if (registration != null) {
            try { registration.close(); } catch (Exception ignored) { }
        }
        Metrics.removeRegistry(registry);
        running = false;
    }

    @Override public boolean isRunning() { return running; }
}
