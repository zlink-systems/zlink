package systems.zlink.framework.spring;

import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@Configuration(proxyBeanMethods = false)
class ZLinkFrameworkEnableConfiguration {
    @Bean
    static ZLinkFrameworkEnabled zlinkFrameworkEnabled() {
        return new ZLinkFrameworkEnabled();
    }
}
