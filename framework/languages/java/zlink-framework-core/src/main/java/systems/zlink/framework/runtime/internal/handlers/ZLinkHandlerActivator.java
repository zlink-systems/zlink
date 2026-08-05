package systems.zlink.framework.runtime.internal.handlers;

import java.lang.reflect.Constructor;
import java.util.LinkedHashMap;
import java.util.Map;
import systems.zlink.framework.errors.ZLinkConfigurationException;

@FunctionalInterface
public interface ZLinkHandlerActivator {
    Object create(Class<?> handlerType);

    default Activation openActivation() {
        ZLinkHandlerActivator activator = this;
        return new Activation() {
            @Override
            public Object create(Class<?> handlerType) {
                return activator.create(handlerType);
            }

            @Override
            public void destroy(Object instance) {
                activator.destroy(instance);
            }

            @Override
            public void close() {
            }
        };
    }

    default void destroy(Object instance) {
        if (instance instanceof AutoCloseable closeable) {
            try {
                closeable.close();
            } catch (RuntimeException error) {
                throw error;
            } catch (Exception error) {
                throw new ZLinkConfigurationException(
                    "failed to destroy handler: " + instance.getClass().getName(),
                    error);
            }
        }
    }

    static ZLinkHandlerActivator reflection() {
        return handlerType -> {
            try {
                return handlerType.getConstructor().newInstance();
            } catch (ReflectiveOperationException ex) {
                throw new ZLinkConfigurationException(
                    "failed to create handler: " + handlerType.getName(),
                    ex);
            }
        };
    }

    static MutableServices services() {
        return new MutableServices(reflection());
    }

    static MutableServices services(ZLinkHandlerActivator fallback) {
        return new MutableServices(fallback);
    }

    interface Activation extends AutoCloseable {
        Object create(Class<?> handlerType);

        default Object create(
            Class<?> handlerType,
            DependencyResolver dependencyResolver) {
            return create(handlerType);
        }

        void destroy(Object instance);

        @Override
        void close();
    }

    @FunctionalInterface
    interface DependencyResolver {
        Object resolve(Class<?> dependencyType);
    }

    final class MutableServices implements ZLinkHandlerActivator {
        private final Map<Class<?>, Object> services = new LinkedHashMap<>();
        private final ZLinkHandlerActivator fallback;

        private MutableServices(ZLinkHandlerActivator fallback) {
            this.fallback = fallback;
        }

        public MutableServices add(Class<?> serviceType, Object service) {
            services.put(serviceType, service);
            return this;
        }

        @Override
        public Object create(Class<?> handlerType) {
            try {
                Object registered = findRuntimeService(handlerType);
                if (registered != null) {
                    return registered;
                }
                for (Constructor<?> constructor : handlerType.getConstructors()) {
                    Object[] arguments = resolveArguments(constructor.getParameterTypes());
                    if (arguments != null && arguments.length > 0) {
                        return constructor.newInstance(arguments);
                    }
                }
                return fallback.create(handlerType);
            } catch (ReflectiveOperationException ex) {
                throw new ZLinkConfigurationException(
                    "failed to create handler: " + handlerType.getName(),
                    ex);
            }
        }

        @Override
        public Activation openActivation() {
            Activation fallbackActivation = fallback.openActivation();
            return new Activation() {
                private final java.util.Set<Object> borrowed =
                    java.util.Collections.newSetFromMap(
                        new java.util.IdentityHashMap<>());

                @Override
                public Object create(Class<?> handlerType) {
                    Object runtimeService = findRuntimeService(handlerType);
                    if (runtimeService != null) {
                        borrowed.add(runtimeService);
                        return runtimeService;
                    }
                    return fallbackActivation.create(
                        handlerType,
                        MutableServices.this::findRuntimeService);
                }

                @Override
                public Object create(
                    Class<?> handlerType,
                    DependencyResolver dependencyResolver) {
                    Object runtimeService = findRuntimeService(handlerType);
                    if (runtimeService != null) {
                        borrowed.add(runtimeService);
                        return runtimeService;
                    }
                    return fallbackActivation.create(
                        handlerType,
                        dependencyType -> {
                            Object service = findRuntimeService(dependencyType);
                            return service != null
                                ? service
                                : dependencyResolver.resolve(dependencyType);
                        });
                }

                @Override
                public void destroy(Object instance) {
                    if (!borrowed.remove(instance)) {
                        fallbackActivation.destroy(instance);
                    }
                }

                @Override
                public void close() {
                    borrowed.clear();
                    fallbackActivation.close();
                }
            };
        }

        private Object[] resolveArguments(Class<?>[] parameterTypes) {
            Object[] arguments = new Object[parameterTypes.length];
            boolean allowFallbackServices = false;
            for (Class<?> parameterType : parameterTypes) {
                if (findRuntimeService(parameterType) != null) {
                    allowFallbackServices = true;
                    break;
                }
            }
            for (int i = 0; i < parameterTypes.length; i++) {
                Object service = findRuntimeService(parameterTypes[i]);
                if (service == null && allowFallbackServices) {
                    service = findFallbackService(parameterTypes[i]);
                }
                if (service == null) {
                    return null;
                }
                arguments[i] = service;
            }
            return arguments;
        }

        private Object findRuntimeService(Class<?> parameterType) {
            for (Map.Entry<Class<?>, Object> entry : services.entrySet()) {
                if (parameterType.isAssignableFrom(entry.getKey())) {
                    return entry.getValue();
                }
            }
            if (fallback instanceof MutableServices parentServices) {
                return parentServices.findRuntimeService(parameterType);
            }
            return null;
        }

        private Object findFallbackService(Class<?> parameterType) {
            try {
                return fallback.create(parameterType);
            } catch (RuntimeException ignored) {
                return null;
            }
        }
    }
}
