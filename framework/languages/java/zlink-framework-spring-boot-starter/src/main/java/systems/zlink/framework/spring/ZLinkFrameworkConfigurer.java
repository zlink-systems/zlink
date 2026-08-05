package systems.zlink.framework.spring;

import systems.zlink.framework.configuration.ZLinkFrameworkOptions;

@FunctionalInterface
public interface ZLinkFrameworkConfigurer {
    void configure(ZLinkFrameworkOptions framework);
}
