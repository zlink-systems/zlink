package systems.zlink.framework.spring;
import java.util.Arrays;

import java.lang.annotation.Annotation;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Parameter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicReference;
import org.springframework.beans.BeansException;
import org.springframework.beans.factory.BeanFactory;
import org.springframework.beans.factory.DisposableBean;
import org.springframework.beans.factory.config.AutowireCapableBeanFactory;
import org.springframework.beans.factory.config.ConfigurableListableBeanFactory;
import org.springframework.beans.factory.config.DependencyDescriptor;
import org.springframework.core.MethodParameter;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkHandlerGroups;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.handlers.ZLinkPublish;
import systems.zlink.framework.handlers.ZLinkRequest;
import systems.zlink.framework.handlers.ZLinkSend;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.handlers.ZLinkSpotRequest;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.handlers.ZLinkStreamPacket;
import systems.zlink.framework.handlers.ZLinkStreamRaw;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

final class ZLinkSpringHandlerFactory implements ZLinkHandlerActivator {
    private final AutowireCapableBeanFactory beanFactory;
    private final Map<Class<?>, HandlerPlan> handlerPlans =
        new ConcurrentHashMap<>();

    ZLinkSpringHandlerFactory(AutowireCapableBeanFactory beanFactory) {
        this.beanFactory = Objects.requireNonNull(beanFactory, "beanFactory");
    }

    @Override
    public void prepare(Class<?> handlerType) {
        ZLinkHandlerActivator.super.prepare(handlerType);
        if (isZLinkManagedType(handlerType)) {
            handlerPlan(handlerType);
        }
    }

    @Override
    public Object create(Class<?> handlerType) {
        if (!isZLinkManagedType(handlerType)) {
            try {
                return beanFactory.getBean(handlerType);
            } catch (BeansException ex) {
                return beanFactory.createBean(handlerType);
            }
        }
        return beanFactory.createBean(handlerType);
    }

    @Override
    public Activation openActivation() {
        return new SpringActivation();
    }

    @Override
    public void destroy(Object instance) {
        destroyOwned(instance);
    }

    private void destroyOwned(Object instance) {
        boolean springInvokesClose = springInvokesClose(instance.getClass());
        beanFactory.destroyBean(instance);
        if (!springInvokesClose && instance instanceof AutoCloseable closeable) {
            try {
                closeable.close();
            } catch (RuntimeException error) {
                throw error;
            } catch (Exception error) {
                throw new IllegalStateException(
                    "failed to close Framework-owned handler: "
                        + instance.getClass().getName(),
                    error);
            }
        }
    }

    private static boolean springInvokesClose(Class<?> type) {
        if (DisposableBean.class.isAssignableFrom(type)) {
            return true;
        }
        for (Class<?> current = type;
             current != null && current != Object.class;
             current = current.getSuperclass()) {
            for (Method method : current.getDeclaredMethods()) {
                if (!method.getName().equals("close")
                    || method.getParameterCount() != 0) {
                    continue;
                }
                for (Annotation annotation : method.getDeclaredAnnotations()) {
                    if (annotation.annotationType().getSimpleName()
                        .equals("PreDestroy")) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    private final class SpringActivation implements Activation {
        private final Map<DependencyKey, Object> scopedDependencies =
            new LinkedHashMap<>();
        private final List<Object> ownedDependencies = new ArrayList<>();
        private boolean closed;

        @Override
        public synchronized Object create(Class<?> handlerType) {
            return create(handlerType, ignored -> null);
        }

        @Override
        public synchronized Object create(
            Class<?> handlerType,
            DependencyResolver dependencyResolver) {
            if (closed) {
                throw new IllegalStateException(
                    "Spring handler activation is closed");
            }
            RuntimeException lastFailure = null;
            for (ConstructorPlan constructor : handlerPlan(handlerType).constructors()) {
                try {
                    Object[] arguments = resolveArguments(
                        constructor,
                        dependencyResolver);
                    Object instance = constructor.constructor().newInstance(
                        arguments);
                    beanFactory.autowireBean(instance);
                    return beanFactory.initializeBean(
                        instance,
                        handlerType.getName() + "#zlinkActivation");
                } catch (BeansException | ReflectiveOperationException failure) {
                    lastFailure = new IllegalStateException(
                        "failed to construct Framework-owned handler: "
                            + handlerType.getName(),
                        unwrap(failure));
                }
            }
            if (lastFailure != null) {
                throw lastFailure;
            }
            return beanFactory.createBean(handlerType);
        }

        private Object[] resolveArguments(
            ConstructorPlan constructor,
            DependencyResolver dependencyResolver) {
            ParameterPlan[] parameters = constructor.parameters();
            Object[] arguments = new Object[parameters.length];
            for (int index = 0; index < parameters.length; index++) {
                ParameterPlan parameter = parameters[index];
                Object supplied = dependencyResolver.resolve(parameter.type());
                if (supplied != null) {
                    arguments[index] = supplied;
                    continue;
                }
                Object cached = scopedDependencies.get(parameter.key());
                if (cached != null) {
                    arguments[index] = cached;
                    continue;
                }
                ShortcutDependencyDescriptor shortcut = parameter.shortcut().get();
                HashSet<String> beanNames = shortcut == null ? new HashSet<>() : null;
                Object dependency = beanFactory.resolveDependency(
                    shortcut == null ? parameter.descriptor() : shortcut,
                    constructor.constructor().getDeclaringClass().getName(),
                    beanNames,
                    null);
                if (dependency == null) {
                    throw new IllegalStateException(
                        "Spring dependency is unavailable: " + parameter.typeName());
                }
                boolean activationScoped = shortcut != null
                    ? shortcut.activationScoped()
                    : beanNames.stream().anyMatch(beanFactory::isPrototype);
                if (activationScoped) {
                    scopedDependencies.put(parameter.key(), dependency);
                    ownedDependencies.add(dependency);
                }
                arguments[index] = dependency;
                if (shortcut == null) {
                    shortcutCandidate(parameter, beanNames)
                        .ifPresent(candidate ->
                            parameter.shortcut().compareAndSet(null, candidate));
                }
            }
            return arguments;
        }

        @Override
        public void destroy(Object instance) {
            destroyOwned(instance);
        }

        @Override
        public void close() {
            List<Object> dependencies;
            synchronized (this) {
                if (closed) {
                    return;
                }
                closed = true;
                dependencies = new ArrayList<>(ownedDependencies);
                ownedDependencies.clear();
                scopedDependencies.clear();
            }
            RuntimeException firstFailure = null;
            for (int index = dependencies.size() - 1; index >= 0; index--) {
                try {
                    destroyOwned(dependencies.get(index));
                } catch (RuntimeException failure) {
                    if (firstFailure == null) {
                        firstFailure = failure;
                    } else {
                        firstFailure.addSuppressed(failure);
                    }
                }
            }
            if (firstFailure != null) {
                throw firstFailure;
            }
        }
    }

    private HandlerPlan handlerPlan(Class<?> handlerType) {
        return handlerPlans.computeIfAbsent(handlerType, this::createHandlerPlan);
    }

    private HandlerPlan createHandlerPlan(Class<?> handlerType) {
        HandlerPlan plan = HandlerPlan.create(handlerType);
        for (ConstructorPlan constructor : plan.constructors()) {
            for (ParameterPlan parameter : constructor.parameters()) {
                preparedShortcut(parameter).ifPresent(shortcut ->
                    parameter.shortcut().compareAndSet(null, shortcut));
            }
        }
        return plan;
    }

    private Optional<ShortcutDependencyDescriptor> preparedShortcut(
        ParameterPlan parameter) {
        if (!(beanFactory instanceof ConfigurableListableBeanFactory configurable)
            || !configurable.isConfigurationFrozen()) {
            return Optional.empty();
        }
        String selected = null;
        for (String beanName : configurable.getBeanNamesForType(
                 parameter.type(), true, false)) {
            if (!configurable.isAutowireCandidate(
                    beanName, parameter.descriptor())) {
                continue;
            }
            if (selected != null) {
                return Optional.empty();
            }
            selected = beanName;
        }
        if (selected == null) {
            return Optional.empty();
        }
        return Optional.of(new ShortcutDependencyDescriptor(
            parameter.descriptor(), selected, beanFactory.isPrototype(selected)));
    }

    private java.util.Optional<ShortcutDependencyDescriptor> shortcutCandidate(
        ParameterPlan parameter,
        HashSet<String> beanNames) {
        if (!(beanFactory instanceof ConfigurableListableBeanFactory configurable)
            || !configurable.isConfigurationFrozen()
            || beanNames.size() != 1) {
            return java.util.Optional.empty();
        }
        String beanName = beanNames.iterator().next();
        if (!beanFactory.containsBean(beanName)
            || !beanFactory.isTypeMatch(beanName, parameter.type())) {
            return java.util.Optional.empty();
        }
        return java.util.Optional.of(new ShortcutDependencyDescriptor(
            parameter.descriptor(),
            beanName,
            beanFactory.isPrototype(beanName)));
    }

    private record HandlerPlan(List<ConstructorPlan> constructors) {
        static HandlerPlan create(Class<?> handlerType) {
            return new HandlerPlan(Arrays.stream(handlerType.getConstructors())
                .sorted(Comparator.<Constructor<?>>comparingInt(
                        ZLinkSpringHandlerFactory::autowiredPriority)
                    .thenComparingInt(Constructor::getParameterCount)
                    .reversed())
                .map(ConstructorPlan::create)
                .toList());
        }
    }

    private record ConstructorPlan(
        Constructor<?> constructor,
        ParameterPlan[] parameters) {
        static ConstructorPlan create(Constructor<?> constructor) {
            Parameter[] parameters = constructor.getParameters();
            ParameterPlan[] plans = new ParameterPlan[parameters.length];
            for (int index = 0; index < parameters.length; index++) {
                plans[index] = new ParameterPlan(
                    parameters[index].getType(),
                    parameters[index].getParameterizedType().getTypeName(),
                    DependencyKey.from(parameters[index]),
                    new DependencyDescriptor(new MethodParameter(constructor, index), true),
                    new AtomicReference<>());
            }
            return new ConstructorPlan(constructor, plans);
        }
    }

    private record ParameterPlan(
        Class<?> type,
        String typeName,
        DependencyKey key,
        DependencyDescriptor descriptor,
        AtomicReference<ShortcutDependencyDescriptor> shortcut) {
    }

    private static final class ShortcutDependencyDescriptor
        extends DependencyDescriptor {
        private final String beanName;
        private final boolean activationScoped;

        private ShortcutDependencyDescriptor(
            DependencyDescriptor descriptor,
            String beanName,
            boolean activationScoped) {
            super(descriptor);
            this.beanName = beanName;
            this.activationScoped = activationScoped;
        }

        private boolean activationScoped() {
            return activationScoped;
        }

        @Override
        public Object resolveShortcut(BeanFactory factory) {
            return factory.getBean(beanName, getDependencyType());
        }
    }

    private static int autowiredPriority(Constructor<?> constructor) {
        for (Annotation annotation : constructor.getDeclaredAnnotations()) {
            if (annotation.annotationType().getSimpleName().equals("Autowired")) {
                return 1;
            }
        }
        return 0;
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof InvocationTargetException invocation
            && invocation.getCause() != null
            ? invocation.getCause()
            : failure;
    }

    private record DependencyKey(
        String type,
        List<String> annotations) {
        static DependencyKey from(Parameter parameter) {
            return new DependencyKey(
                parameter.getParameterizedType().getTypeName(),
                Arrays.stream(parameter.getDeclaredAnnotations())
                    .map(Object::toString)
                    .sorted()
                    .toList());
        }
    }

    private static boolean isZLinkManagedType(Class<?> type) {
        return ZLinkHandlerFilter.class.isAssignableFrom(type)
            || ZLinkActorFactory.class.isAssignableFrom(type)
            || ZLinkSendHandler.class.isAssignableFrom(type)
            || ZLinkRequestHandler.class.isAssignableFrom(type)
            || ZLinkFanoutHandler.class.isAssignableFrom(type)
            || ZLinkRouteSendHandler.class.isAssignableFrom(type)
            || ZLinkRouteRequestHandler.class.isAssignableFrom(type)
            || ZLinkSpot.class.isAssignableFrom(type)
            || ZLinkEntrySpot.class.isAssignableFrom(type)
            || ZLinkSpotPacketHandler.class.isAssignableFrom(type)
            || ZLinkSpotRequestHandler.class.isAssignableFrom(type)
            || ZLinkSpotSubscriptionHandler.class.isAssignableFrom(type)
            || ZLinkSpotTimerHandler.class.isAssignableFrom(type)
            || ZLinkEntrySpotActorSendHandler.class.isAssignableFrom(type)
            || ZLinkEntrySpotActorRequestHandler.class.isAssignableFrom(type)
            || ZLinkSpotActorSendHandler.class.isAssignableFrom(type)
            || ZLinkSpotActorRequestHandler.class.isAssignableFrom(type)
            || ZLinkSession.class.isAssignableFrom(type)
            || ZLinkTypedSessionPacketHandler.class.isAssignableFrom(type)
            || hasZLinkHandlerAnnotation(type);
    }

    private static boolean hasZLinkHandlerAnnotation(Class<?> type) {
        if (type.isAnnotationPresent(ZLinkHandlerGroup.class)
            || type.isAnnotationPresent(ZLinkHandlerGroups.class)) {
            return true;
        }
        for (Method method : type.getDeclaredMethods()) {
            for (Annotation annotation : method.getDeclaredAnnotations()) {
                if (annotation.annotationType() == ZLinkSend.class
                    || annotation.annotationType() == ZLinkRequest.class
                    || annotation.annotationType() == ZLinkPublish.class
                    || annotation.annotationType() == ZLinkPacket.class
                    || annotation.annotationType() == ZLinkStreamPacket.class
                    || annotation.annotationType() == ZLinkStreamRaw.class
                    || annotation.annotationType() == ZLinkSpotSubscription.class
                    || annotation.annotationType() == ZLinkSpotRequest.class
                    || annotation.annotationType() == ZLinkSpotActorSend.class
                    || annotation.annotationType() == ZLinkSpotActorRequest.class) {
                    return true;
                }
            }
        }
        return false;
    }
}
