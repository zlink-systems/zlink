package systems.zlink.framework.spring;

import org.springframework.beans.factory.config.ConfigurableListableBeanFactory;
import org.springframework.beans.factory.support.AbstractBeanDefinition;
import org.springframework.beans.factory.support.BeanDefinitionRegistry;
import org.springframework.beans.factory.support.RootBeanDefinition;

final class ZLinkSpringBeanDefinitions {
    private ZLinkSpringBeanDefinitions() {
    }

    static boolean hasBean(
        ConfigurableListableBeanFactory beanFactory,
        Class<?> beanType) {
        return beanFactory.getBeanNamesForType(beanType, true, false).length > 0;
    }

    static void registerDelegate(
        BeanDefinitionRegistry registry,
        String beanName,
        Class<?> beanClass) {
        if (registry.containsBeanDefinition(beanName)) {
            return;
        }
        RootBeanDefinition definition = new RootBeanDefinition(beanClass);
        definition.setAutowireMode(AbstractBeanDefinition.AUTOWIRE_CONSTRUCTOR);
        registry.registerBeanDefinition(beanName, definition);
    }
}
