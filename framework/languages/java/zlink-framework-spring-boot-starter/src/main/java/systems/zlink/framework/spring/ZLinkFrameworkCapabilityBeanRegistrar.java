package systems.zlink.framework.spring;

import org.springframework.beans.BeansException;
import org.springframework.beans.factory.config.BeanFactoryPostProcessor;
import org.springframework.beans.factory.config.ConfigurableListableBeanFactory;
import org.springframework.beans.factory.support.BeanDefinitionRegistry;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class ZLinkFrameworkCapabilityBeanRegistrar implements BeanFactoryPostProcessor {
    @Override
    public void postProcessBeanFactory(ConfigurableListableBeanFactory beanFactory)
        throws BeansException {
        if (!(beanFactory instanceof BeanDefinitionRegistry registry)) {
            return;
        }

        DefaultZLinkFrameworkOptions options =
            beanFactory.getBean(DefaultZLinkFrameworkOptions.class);
        ZLinkSessionPacketHandlerDiscovery.discover(options);
        options.validate();
        ZLinkApplicationBeanRegistrar.register(registry, beanFactory, options);
        ZLinkFrameworkCapabilityDelegates.register(registry, beanFactory, options);
    }
}
