package systems.zlink.framework.spring;

import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.Metrics;

import org.springframework.context.SmartLifecycle;

import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.util.List;

final class ZLinkMetricsLifecycle implements SmartLifecycle {
    private final MeterRegistry registry;
    private ZLinkRuntimeMetrics.Registration registration;
    private volatile boolean running;

    ZLinkMetricsLifecycle(MeterRegistry registry, List<ZLinkMetricsCustomizer> customizers) {
        this.registry = registry;
        for (ZLinkMetricsCustomizer customizer : customizers) {
            customizer.customize(registry);
        }
    }

    @Override
    public int getPhase() {
        // Collect before Framework startup and through its terminal shutdown state.
        return systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle.PHASE - 1;
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
            registration.close();
        }
        Metrics.removeRegistry(registry);
        running = false;
    }

    @Override
    public boolean isRunning() {
        return running;
    }
}
