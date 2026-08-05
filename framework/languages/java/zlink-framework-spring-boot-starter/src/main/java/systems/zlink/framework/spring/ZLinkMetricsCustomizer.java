package systems.zlink.framework.spring;

import io.micrometer.core.instrument.MeterRegistry;

@FunctionalInterface
public interface ZLinkMetricsCustomizer {
    void customize(MeterRegistry registry);
}
