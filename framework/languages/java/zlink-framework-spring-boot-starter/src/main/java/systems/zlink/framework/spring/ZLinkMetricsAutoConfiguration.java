package systems.zlink.framework.spring;

import io.micrometer.core.instrument.MeterRegistry;

import org.springframework.boot.autoconfigure.AutoConfiguration;
import org.springframework.boot.autoconfigure.condition.ConditionalOnBean;
import org.springframework.boot.autoconfigure.condition.ConditionalOnClass;
import org.springframework.boot.autoconfigure.condition.ConditionalOnMissingBean;
import org.springframework.context.annotation.Bean;

import java.util.List;

@AutoConfiguration(after = ZLinkFrameworkAutoConfiguration.class)
@ConditionalOnClass(MeterRegistry.class)
public class ZLinkMetricsAutoConfiguration {
    @Bean
    @ConditionalOnBean({ZLinkFrameworkEnabled.class, MeterRegistry.class})
    @ConditionalOnMissingBean
    ZLinkMetricsLifecycle zlinkMetricsLifecycle(
            MeterRegistry registry, List<ZLinkMetricsCustomizer> customizers) {
        return new ZLinkMetricsLifecycle(registry, customizers);
    }
}
