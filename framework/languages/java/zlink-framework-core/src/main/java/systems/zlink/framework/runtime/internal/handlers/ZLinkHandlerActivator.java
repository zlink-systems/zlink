package systems.zlink.framework.runtime.internal.handlers;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.lang.reflect.Constructor;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.BiPredicate;

@FunctionalInterface
public interface ZLinkHandlerActivator {
    Object create(Class<?> handlerType);

    default Object findService(Class<?> serviceType) {
        return create(serviceType);
    }

    default Object findService(Constructor<?> constructor, int parameterIndex) {
        return findService(PublicConstructorPlan.parameterType(constructor, parameterIndex));
    }

    /** Prepares reusable construction metadata without creating an instance. */
    default void prepare(Class<?> handlerType) {
        PublicConstructorPlan.prepare(handlerType);
    }

    /** Compiles against service keys without retaining their scope or instances. */
    default PreparedConstruction prepareConstruction(Class<?> handlerType, Set<Class<?>> runtimeServices) {
        prepare(handlerType);
        ConstructionPlan plan = PublicConstructorPlan.forType(handlerType)
                .compile(runtimeServices, this::isServiceAvailable, false);
        return plan == null ? (activation, resolver) -> activation.create(handlerType, resolver)
                : plan.bind();
    }

    /** Application-owned factories have no inspectable dependency catalog. */
    default boolean isServiceAvailable(Constructor<?> constructor, int parameterIndex) {
        return true;
    }

    @FunctionalInterface
    interface PreparedConstruction {
        Object create(Activation activation, DependencyResolver resolver);
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
            public Object findService(Constructor<?> constructor, int index) {
                return activator.findService(constructor, index);
            }

            @Override
            public void destroy(Object instance) {
                activator.destroy(instance);
            }

            @Override
            public void close() {}
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
                        "failed to destroy handler: " + instance.getClass().getName(), error);
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

        default Object create(Class<?> handlerType, DependencyResolver dependencyResolver) {
            return create(handlerType);
        }

        default Object findService(Constructor<?> constructor, int index) {
            return create(PublicConstructorPlan.parameterType(constructor, index));
        }

        default Object createPrepared(PreparedConstruction construction, DependencyResolver resolver) {
            return construction.create(this, resolver);
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
        private final Map<Class<?>, Map<Set<Class<?>>, PreparedConstruction>> constructionPlans =
                new ConcurrentHashMap<>();

        private MutableServices(ZLinkHandlerActivator fallback) {
            this.fallback = fallback;
        }

        public synchronized MutableServices add(Class<?> serviceType, Object service) {
            services.put(
                    Objects.requireNonNull(serviceType, "serviceType"),
                    Objects.requireNonNull(service, "service"));
            serviceIndex = buildServiceIndex(services);
            constructionPlans.clear();
            return this;
        }

        @Override
        public void prepare(Class<?> handlerType) {
            if (findRuntimeService(handlerType) == null) constructionPlan(handlerType);
        }

        @Override
        public PreparedConstruction prepareConstruction(Class<?> handlerType, Set<Class<?>> runtimeServices) {
            Set<Class<?>> available = new HashSet<>(runtimeServices);
            available.addAll(serviceIndex.keySet());
            PreparedConstruction plan = constructionPlan(handlerType, Set.copyOf(available));
            return (activation, resolver) -> activation.createPrepared(plan, resolver);
        }

        private PreparedConstruction constructionPlan(Class<?> handlerType) {
            return constructionPlan(handlerType, serviceIndex.keySet());
        }

        private PreparedConstruction constructionPlan(Class<?> handlerType, Set<Class<?>> available) {
            return constructionPlans.computeIfAbsent(handlerType, ignored -> new ConcurrentHashMap<>())
                    .computeIfAbsent(available, keys -> fallback.prepareConstruction(handlerType, keys));
        }

        @Override
        public Object create(Class<?> handlerType) {
            Object registered = findRuntimeService(handlerType);
            if (registered != null) return registered;
            return constructionPlan(handlerType).create(
                    fallback.openActivation(), this::findRuntimeService);
        }

        @Override
        public Object findService(Class<?> serviceType) {
            Object registered = findRuntimeService(serviceType);
            return registered != null ? registered : fallback.findService(serviceType);
        }

        @Override
        public Object findService(Constructor<?> constructor, int parameterIndex) {
            Object registered = findRuntimeService(PublicConstructorPlan.parameterType(constructor, parameterIndex));
            return registered != null
                    ? registered
                    : fallback.findService(constructor, parameterIndex);
        }

        @Override
        public Activation openActivation() {
            return new ServicesActivation(fallback.openActivation());
        }

        private final class ServicesActivation implements Activation {
            private final Activation fallbackActivation;

            private ServicesActivation(Activation fallbackActivation) {
                this.fallbackActivation = fallbackActivation;
            }

            @Override
            public Object createPrepared(PreparedConstruction plan, DependencyResolver resolver) {
                return fallbackActivation.createPrepared(plan, resolver);
            }

            private final Set<Object> borrowed =
                    Collections.newSetFromMap(new IdentityHashMap<>());

            @Override
            public Object create(Class<?> handlerType) {
                Object runtimeService = findRuntimeService(handlerType);
                if (runtimeService != null) {
                    borrowed.add(runtimeService);
                    return runtimeService;
                }
                return constructionPlan(handlerType).create(
                        fallbackActivation, MutableServices.this::findRuntimeService);
            }

            @Override
            public Object create(Class<?> handlerType, DependencyResolver dependencyResolver) {
                Object runtimeService = findRuntimeService(handlerType);
                if (runtimeService != null) {
                    borrowed.add(runtimeService);
                    return runtimeService;
                }
                return constructionPlan(handlerType).create(
                        fallbackActivation, dependencyType -> {
                            Object service = findRuntimeService(dependencyType);
                            return service != null ? service : dependencyResolver.resolve(dependencyType);
                        });
            }

            @Override
            public Object findService(Constructor<?> constructor, int index) {
                return fallbackActivation.findService(constructor, index);
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

        private static Map<Class<?>, Object> buildServiceIndex(Map<Class<?>, Object> services) {
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
    }
}

final class ReflectionActivator implements ZLinkHandlerActivator {
    private static final ClassValue<ConstructionPlan> PLANS = new ClassValue<>() {
        @Override
        protected ConstructionPlan computeValue(Class<?> type) {
            return PublicConstructorPlan.forType(type).compile(Set.of(), (constructor, index) -> false, true);
        }
    };

    @Override
    public void prepare(Class<?> handlerType) {
        requiredPlan(handlerType);
    }

    @Override
    public PreparedConstruction prepareConstruction(Class<?> type, Set<Class<?>> runtimeServices) {
        ConstructionPlan plan = PublicConstructorPlan.forType(type)
                .compile(runtimeServices, this::isServiceAvailable, true);
        if (plan == null) throw new ZLinkConfigurationException(
                "No available public constructor: " + type.getName());
        return plan.bind();
    }

    @Override
    public boolean isServiceAvailable(Constructor<?> constructor, int index) {
        return PLANS.get(PublicConstructorPlan.parameterType(constructor, index)) != null;
    }

    @Override
    public Object create(Class<?> handlerType) {
        return requiredPlan(handlerType).create((constructor, index, type) -> null);
    }

    @Override
    public Object findService(Class<?> serviceType) {
        ConstructionPlan plan = PLANS.get(serviceType);
        return plan == null ? null : plan.create((constructor, index, type) -> null);
    }

    private ConstructionPlan requiredPlan(Class<?> type) {
        ConstructionPlan plan = PLANS.get(type);
        if (plan == null) throw new ZLinkConfigurationException(
                "No available public constructor: " + type.getName());
        return plan;
    }
}

record PublicConstructorPlan(Class<?> handlerType, Map<Constructor<?>, Class<?>[]> constructors) {
    private static final ClassValue<PublicConstructorPlan> PLANS = new ClassValue<>() {
        @Override
        protected PublicConstructorPlan computeValue(Class<?> type) {
            Map<Constructor<?>, Class<?>[]> constructors = new LinkedHashMap<>();
            Arrays.stream(type.getConstructors())
                    .sorted(Comparator.<Constructor<?>>comparingInt(
                            Constructor::getParameterCount).reversed())
                    .forEach(constructor -> constructors.put(constructor, constructor.getParameterTypes()));
            return new PublicConstructorPlan(type, Collections.unmodifiableMap(constructors));
        }
    };

    static void prepare(Class<?> type) { forType(type); }

    static PublicConstructorPlan forType(Class<?> type) {
        return PLANS.get(Objects.requireNonNull(type, "handlerType"));
    }

    static Class<?> parameterType(Constructor<?> constructor, int index) {
        return forType(constructor.getDeclaringClass()).constructors().get(constructor)[index];
    }

    ConstructionPlan compile(Set<Class<?>> runtimeServices,
            BiPredicate<Constructor<?>, Integer> available, boolean allowNoArg) {
        if (java.lang.reflect.Modifier.isAbstract(handlerType.getModifiers())) return null;
        ConstructionPlan selected = null;
        for (Map.Entry<Constructor<?>, Class<?>[]> candidate : constructors.entrySet()) {
            Constructor<?> constructor = candidate.getKey();
            Class<?>[] types = candidate.getValue();
            boolean[] runtimeArguments = new boolean[types.length];
            boolean hasRuntimeArgument = false;
            for (int index = 0; index < types.length; index++) {
                runtimeArguments[index] = runtimeServices.contains(types[index]);
                hasRuntimeArgument |= runtimeArguments[index];
            }
            if (!hasRuntimeArgument && !(allowNoArg && types.length == 0)) continue;
            boolean supplied = true;
            for (int index = 0; index < types.length; index++) {
                if (!runtimeArguments[index] && !available.test(constructor, index)) {
                    supplied = false;
                    break;
                }
            }
            if (!supplied) continue;
            if (selected != null) {
                if (selected.parameterTypes().length == types.length) {
                    throw new ZLinkConfigurationException(
                            "Ambiguous public constructors: " + handlerType.getName());
                }
                break;
            }
            selected = new ConstructionPlan(constructor, types, runtimeArguments);
        }
        return selected;
    }
}

record ConstructionPlan(
        Constructor<?> constructor, Class<?>[] parameterTypes, boolean[] runtimeArguments) {
    interface ArgumentResolver {
        Object resolve(Constructor<?> constructor, int index, Class<?> type);
    }

    ZLinkHandlerActivator.PreparedConstruction bind() {
        return (activation, resolver) -> create((selected, index, type) -> {
            if (!runtimeArguments[index]) return activation.findService(selected, index);
            Object dependency = resolver.resolve(type);
            if (dependency == null) throw new ZLinkConfigurationException(
                    "Runtime dependency is unavailable: " + type.getName());
            return dependency;
        });
    }

    Object create(ArgumentResolver resolver) {
        Object[] arguments = new Object[parameterTypes.length];
        for (int index = 0; index < arguments.length; index++) {
            arguments[index] = resolver.resolve(constructor, index, parameterTypes[index]);
        }
        try {
            return constructor.newInstance(arguments);
        } catch (ReflectiveOperationException failure) {
            throw new ZLinkConfigurationException(
                    "failed to create handler: " + constructor.getDeclaringClass().getName(), failure);
        }
    }
}
