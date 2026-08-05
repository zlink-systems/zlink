package systems.zlink.framework.spring;

import java.beans.Introspector;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.util.Collection;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import org.springframework.beans.factory.config.BeanDefinition;
import org.springframework.beans.factory.config.ConfigurableListableBeanFactory;
import org.springframework.beans.factory.support.AbstractBeanDefinition;
import org.springframework.beans.factory.support.BeanDefinitionRegistry;
import org.springframework.beans.factory.support.RootBeanDefinition;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class ZLinkApplicationBeanRegistrar {
    private ZLinkApplicationBeanRegistrar() {
    }

    static void register(
        BeanDefinitionRegistry registry,
        ConfigurableListableBeanFactory beanFactory,
        DefaultZLinkFrameworkOptions options) {
        Set<Class<?>> applicationTypes = new LinkedHashSet<>(options.registration().applicationTypes());
        for (Class<?> applicationType : List.copyOf(applicationTypes)) {
            applicationTypes.addAll(findCollectionDependencyImplementations(applicationType));
        }
        for (Class<?> applicationType : applicationTypes) {
            registerPrototypeIfMissing(registry, beanFactory, applicationType);
        }
    }

    private static Set<Class<?>> findCollectionDependencyImplementations(Class<?> applicationType) {
        Set<Class<?>> implementations = new LinkedHashSet<>();
        for (var constructor : applicationType.getConstructors()) {
            for (Type parameter : constructor.getGenericParameterTypes()) {
                Class<?> serviceType = collectionElementType(parameter);
                if (serviceType == null || !serviceType.isInterface()) {
                    continue;
                }
                implementations.addAll(
                    ZLinkClasspathTypeScanner.findAssignableTypes(
                        applicationType.getPackageName(),
                        serviceType));
                implementations.addAll(
                    ZLinkClasspathTypeScanner.findAssignableTypes(
                        serviceType.getPackageName(),
                        serviceType));
            }
        }
        implementations.remove(applicationType);
        return implementations;
    }

    private static Class<?> collectionElementType(Type parameter) {
        if (!(parameter instanceof ParameterizedType parameterized)
            || !(parameterized.getRawType() instanceof Class<?> rawType)
            || !(rawType == List.class
                || rawType == Collection.class
                || rawType == Iterable.class
                || rawType == Set.class)) {
            return null;
        }
        Type argument = parameterized.getActualTypeArguments()[0];
        return argument instanceof Class<?> elementType ? elementType : null;
    }

    private static void registerPrototypeIfMissing(
        BeanDefinitionRegistry registry,
        ConfigurableListableBeanFactory beanFactory,
        Class<?> beanClass) {
        if (ZLinkSpringBeanDefinitions.hasBean(beanFactory, beanClass)) {
            return;
        }
        RootBeanDefinition definition = new RootBeanDefinition(beanClass);
        definition.setAutowireMode(AbstractBeanDefinition.AUTOWIRE_CONSTRUCTOR);
        definition.setScope(BeanDefinition.SCOPE_PROTOTYPE);
        registry.registerBeanDefinition(
            "zlinkApplication." + Introspector.decapitalize(beanClass.getName()),
            definition);
    }
}
