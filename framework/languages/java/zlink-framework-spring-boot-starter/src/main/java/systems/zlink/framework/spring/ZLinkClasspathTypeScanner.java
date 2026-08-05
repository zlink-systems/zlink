package systems.zlink.framework.spring;

import java.util.LinkedHashSet;
import java.util.Set;
import org.springframework.beans.factory.config.BeanDefinition;
import org.springframework.context.annotation.ClassPathScanningCandidateComponentProvider;
import org.springframework.core.type.filter.AssignableTypeFilter;

final class ZLinkClasspathTypeScanner {
    private ZLinkClasspathTypeScanner() {
    }

    static Set<Class<?>> findAssignableTypes(String packageName, Class<?> serviceType) {
        ClassPathScanningCandidateComponentProvider scanner =
            new ClassPathScanningCandidateComponentProvider(false);
        scanner.addIncludeFilter(new AssignableTypeFilter(serviceType));
        Set<Class<?>> types = new LinkedHashSet<>();
        for (BeanDefinition candidate : scanner.findCandidateComponents(packageName)) {
            String className = candidate.getBeanClassName();
            if (className == null) {
                continue;
            }
            try {
                Class<?> type = Class.forName(className, false, contextClassLoader(serviceType));
                if (type != serviceType && serviceType.isAssignableFrom(type)) {
                    types.add(type);
                }
            } catch (ClassNotFoundException ignored) {
                // Optional package contents should not make framework registration unusable.
            }
        }
        return types;
    }

    static Set<Class<?>> findCandidateTypes(String packageName, Class<?> fallbackType) {
        ClassPathScanningCandidateComponentProvider scanner =
            new ClassPathScanningCandidateComponentProvider(false);
        scanner.addIncludeFilter((metadataReader, metadataReaderFactory) -> true);
        Set<Class<?>> types = new LinkedHashSet<>();
        for (BeanDefinition candidate : scanner.findCandidateComponents(packageName)) {
            String className = candidate.getBeanClassName();
            if (className == null) {
                continue;
            }
            try {
                types.add(Class.forName(className, false, contextClassLoader(fallbackType)));
            } catch (ClassNotFoundException ignored) {
                // Optional package contents should not make framework registration unusable.
            }
        }
        return types;
    }

    private static ClassLoader contextClassLoader(Class<?> fallbackType) {
        ClassLoader loader = Thread.currentThread().getContextClassLoader();
        return loader == null ? fallbackType.getClassLoader() : loader;
    }
}
