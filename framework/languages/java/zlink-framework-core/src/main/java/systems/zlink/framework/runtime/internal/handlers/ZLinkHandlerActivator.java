package systems.zlink.framework.runtime.internal.handlers;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Set;

import java.lang.reflect.Constructor;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import systems.zlink.framework.errors.ZLinkConfigurationException;

@FunctionalInterface
public interface ZLinkHandlerActivator {
    Object create(Class<?> handlerType);

    /** Prepares reusable construction metadata without creating an instance. */
    default void prepare(Class<?> handlerType) {
        PublicConstructorPlan.prepare(handlerType);
    }

    default void prepare(Iterable<Class<?>> handlerTypes) {
        Objects.requireNonNull(handlerTypes, "handlerTypes");
        handlerTypes.forEach(this::prepare);
    }

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
        return new ReflectionActivator();
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
        private volatile Map<Class<?>, Object> serviceIndex = Map.of();
        private final ZLinkHandlerActivator fallback;

        private MutableServices(ZLinkHandlerActivator fallback) {
            this.fallback = fallback;
        }

        public synchronized MutableServices add(Class<?> serviceType, Object service) {
            services.put(
                Objects.requireNonNull(serviceType, "serviceType"),
                Objects.requireNonNull(service, "service"));
            serviceIndex = buildServiceIndex(services);
            return this;
        }

        @Override
        public void prepare(Class<?> handlerType) {
            ZLinkHandlerActivator.super.prepare(handlerType);
            if (findRuntimeService(handlerType) == null) {
                fallback.prepare(handlerType);
            }
        }

        @Override
        public Object create(Class<?> handlerType) {
            try {
                Object registered = findRuntimeService(handlerType);
                if (registered != null) {
                    return registered;
                }
                for (Constructor<?> constructor :
                    PublicConstructorPlan.forType(handlerType).constructors()) {
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
                private final Set<Object> borrowed =
                    Collections.newSetFromMap(
                        new IdentityHashMap<>());

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
            Object service = serviceIndex.get(parameterType);
            if (service != null) {
                return service;
            }
            if (fallback instanceof MutableServices parentServices) {
                return parentServices.findRuntimeService(parameterType);
            }
            return null;
        }

        private static Map<Class<?>, Object> buildServiceIndex(
            Map<Class<?>, Object> services) {
            Map<Class<?>, Object> index = new HashMap<>();
            for (Map.Entry<Class<?>, Object> entry : services.entrySet()) {
                ArrayDeque<Class<?>> pending = new ArrayDeque<>();
                HashSet<Class<?>> visited = new HashSet<>();
                pending.add(entry.getKey());
                while (!pending.isEmpty()) {
                    Class<?> type = pending.removeFirst();
                    if (!visited.add(type)) {
                        continue;
                    }
                    index.putIfAbsent(type, entry.getValue());
                    Class<?> parent = type.getSuperclass();
                    if (parent != null) {
                        pending.addLast(parent);
                    } else if (type.isInterface()) {
                        pending.addLast(Object.class);
                    }
                    for (Class<?> contract : type.getInterfaces()) {
                        pending.addLast(contract);
                    }
                }
            }
            return Map.copyOf(index);
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

final class ReflectionActivator implements ZLinkHandlerActivator {
    @Override
    public Object create(Class<?> handlerType) {
        return PublicConstructorPlan.forType(handlerType).createNoArg();
    }
}

record PublicConstructorPlan(
    Class<?> handlerType,
    List<Constructor<?>> constructors,
    Constructor<?> noArgConstructor) {
    private static final ClassValue<PublicConstructorPlan> PLANS =
        new ClassValue<>() {
            @Override
            protected PublicConstructorPlan computeValue(Class<?> handlerType) {
                List<Constructor<?>> constructors =
                    List.of(handlerType.getConstructors());
                Constructor<?> noArg = null;
                for (Constructor<?> constructor : constructors) {
                    if (constructor.getParameterCount() == 0) {
                        noArg = constructor;
                        break;
                    }
                }
                return new PublicConstructorPlan(handlerType, constructors, noArg);
            }
        };

    static void prepare(Class<?> handlerType) {
        forType(handlerType);
    }

    static PublicConstructorPlan forType(Class<?> handlerType) {
        return PLANS.get(Objects.requireNonNull(handlerType, "handlerType"));
    }

    Object createNoArg() {
        if (noArgConstructor == null) {
            throw failure(new NoSuchMethodException(
                handlerType.getName() + ".<init>()"));
        }
        try {
            return noArgConstructor.newInstance();
        } catch (ReflectiveOperationException failure) {
            throw failure(failure);
        }
    }

    private ZLinkConfigurationException failure(
        ReflectiveOperationException cause) {
        return new ZLinkConfigurationException(
            "failed to create handler: " + handlerType.getName(), cause);
    }
}
