package systems.zlink.framework.spring;

import org.springframework.beans.BeansException;
import org.springframework.beans.factory.BeanFactory;
import org.springframework.beans.factory.BeanFactoryUtils;
import org.springframework.beans.factory.DisposableBean;
import org.springframework.beans.factory.NoSuchBeanDefinitionException;
import org.springframework.beans.factory.NoUniqueBeanDefinitionException;
import org.springframework.beans.factory.ObjectFactory;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.config.AutowireCapableBeanFactory;
import org.springframework.beans.factory.config.BeanDefinition;
import org.springframework.beans.factory.config.ConfigurableListableBeanFactory;
import org.springframework.beans.factory.config.DependencyDescriptor;
import org.springframework.beans.factory.support.AutowireCandidateResolver;
import org.springframework.beans.factory.support.DefaultListableBeanFactory;
import org.springframework.core.DefaultParameterNameDiscoverer;
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

import java.lang.annotation.Annotation;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Parameter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkSpringHandlerFactory implements ZLinkHandlerActivator {
    private static final String CLOSE_METHOD_NAME = "close";
    private static final String PRE_DESTROY_ANNOTATION_CLASS_NAME = "jakarta.annotation.PreDestroy";
    private final AutowireCapableBeanFactory beanFactory;
    private final Map<Class<?>, HandlerPlan> handlerPlans = new ConcurrentHashMap<>();

    ZLinkSpringHandlerFactory(AutowireCapableBeanFactory beanFactory) {
        this.beanFactory = Objects.requireNonNull(beanFactory, "beanFactory");
    }

    @Override
    public void prepare(Class<?> handlerType) {
        ZLinkHandlerActivator.super.prepare(handlerType);
        if (isZLinkManagedType(handlerType)) {
            HandlerPlan plan = handlerPlan(handlerType);
            RuntimeException failure = null;
            for (ConstructorPlan constructor : plan.constructors()) {
                try {
                    for (ParameterPlan parameter : constructor.parameters()) {
                        validateDependency(handlerType, parameter.descriptor());
                    }
                    return;
                } catch (NoSuchBeanDefinitionException error) {
                    failure = error;
                }
            }
            if (failure != null) {
                throw failure;
            }
        }
    }

    private void validateDependency(Class<?> owner, DependencyDescriptor parameter) {
        Class<?> type = parameter.getDependencyType();
        if ((ZLinkSpot.class.isAssignableFrom(owner)
                        && type == systems.zlink.framework.spots.ZLinkSpotContext.class)
                || (ZLinkEntrySpot.class.isAssignableFrom(owner)
                        && type == systems.zlink.framework.spots.ZLinkEntrySpotContext.class)
                || (systems.zlink.framework.spots.ZLinkInstanceSpot.class.isAssignableFrom(owner)
                        && type == systems.zlink.framework.spots.ZLinkInstanceSpotContext.class)
                || ((ZLinkSession.class.isAssignableFrom(owner)
                                || owner.isAnnotationPresent(ZLinkStreamPacket.class)
                                || owner.isAnnotationPresent(ZLinkStreamRaw.class))
                        && (type == systems.zlink.framework.streams.ZLinkSessionContext.class
                                || type
                                        == systems.zlink.framework.streams
                                                .ZLinkSessionPacketDispatcher.class))) {
            return;
        }
        if (!(beanFactory instanceof DefaultListableBeanFactory source)) {
            throw new IllegalStateException(
                    "Spring startup validation requires a metadata bean factory");
        }
        var resolver = source.getAutowireCandidateResolver();
        if (resolver.getSuggestedValue(parameter) != null
                || type == ObjectProvider.class
                || type == ObjectFactory.class
                || type.isArray()
                || java.util.Collection.class.isAssignableFrom(type)
                || java.util.Map.class.isAssignableFrom(type)) {
            return;
        }
        DependencyDescriptor descriptor = new DependencyDescriptor(parameter);
        descriptor.initParameterNameDiscovery(new DefaultParameterNameDiscoverer());
        if (type == Optional.class) {
            descriptor.increaseNestingLevel();
        }
        Map<String, Object> candidates = new java.util.LinkedHashMap<>();
        for (String name :
                BeanFactoryUtils.beanNamesForTypeIncludingAncestors(
                        source, descriptor.getResolvableType(), true, false)) {
            if (source.isAutowireCandidate(name, descriptor)) {
                candidates.put(name, source.getType(name, false));
            }
        }
        if (candidates.isEmpty()) {
            if (resolver.isRequired(parameter)) {
                throw new NoSuchBeanDefinitionException(descriptor.getResolvableType());
            }
        } else if (candidates.size() > 1
                && new MetadataCandidateSelector(source).select(candidates, descriptor) == null) {
            throw new NoUniqueBeanDefinitionException(
                    descriptor.getResolvableType(), candidates.keySet());
        }
    }

    /** Spring owns candidate selection; this view exposes only bean definition metadata. */
    private static final class MetadataCandidateSelector extends DefaultListableBeanFactory {
        private final DefaultListableBeanFactory source;

        MetadataCandidateSelector(DefaultListableBeanFactory source) {
            super(source.getParentBeanFactory());
            this.source = source;
            setDependencyComparator(source.getDependencyComparator());
        }

        String select(Map<String, Object> candidates, DependencyDescriptor descriptor) {
            return determineAutowireCandidate(candidates, descriptor);
        }

        @Override
        public BeanDefinition getBeanDefinition(String name) {
            return source.getBeanDefinition(name);
        }

        @Override
        public boolean containsBeanDefinition(String name) {
            return source.containsBeanDefinition(name);
        }

        @Override
        public String[] getAliases(String name) {
            return source.getAliases(name);
        }

        @Override
        public AutowireCandidateResolver getAutowireCandidateResolver() {
            return source.getAutowireCandidateResolver();
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
    public Object findService(Class<?> serviceType) {
        return beanFactory.getBeanProvider(serviceType).getIfAvailable();
    }

    @Override
    public Object findService(Constructor<?> constructor, int parameterIndex) {
        return beanFactory.resolveDependency(
                new DependencyDescriptor(new MethodParameter(constructor, parameterIndex), false),
                constructor.getDeclaringClass().getName());
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
                        "failed to close Framework-owned handler: " + instance.getClass().getName(),
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
                if (!method.getName().equals(CLOSE_METHOD_NAME)
                        || method.getParameterCount() != 0) {
                    continue;
                }
                for (Annotation annotation : method.getDeclaredAnnotations()) {
                    if (annotation
                            .annotationType()
                            .getName()
                            .equals(PRE_DESTROY_ANNOTATION_CLASS_NAME)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    private final class SpringActivation implements Activation {
        private final Map<Object, Object> scopedDependencies = new LinkedHashMap<>();
        private final List<Object> ownedDependencies = new ArrayList<>();
        private boolean closed;

        @Override
        public synchronized Object create(Class<?> handlerType) {
            return create(handlerType, ignored -> null);
        }

        @Override
        public synchronized Object create(
                Class<?> handlerType, DependencyResolver dependencyResolver) {
            if (closed) {
                throw new IllegalStateException("Spring handler activation is closed");
            }
            RuntimeException lastFailure = null;
            for (ConstructorPlan constructor : handlerPlan(handlerType).constructors()) {
                try {
                    Object[] arguments = resolveArguments(constructor, dependencyResolver);
                    Object instance = constructor.constructor().newInstance(arguments);
                    beanFactory.autowireBean(instance);
                    return beanFactory.initializeBean(
                            instance, handlerType.getName() + "#zlinkActivation");
                } catch (BeansException | ReflectiveOperationException failure) {
                    lastFailure =
                            new IllegalStateException(
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
                ConstructorPlan constructor, DependencyResolver dependencyResolver) {
            ParameterPlan[] parameters = constructor.parameters();
            Object[] arguments = new Object[parameters.length];
            for (int index = 0; index < parameters.length; index++) {
                ParameterPlan parameter = parameters[index];
                Object supplied = dependencyResolver.resolve(parameter.type());
                if (supplied != null) {
                    arguments[index] = supplied;
                    continue;
                }
                Object aggregate = scopedDependencies.get(parameter.key());
                if (aggregate != null) {
                    arguments[index] = aggregate;
                    continue;
                }
                String shortcut = parameter.shortcut().get();
                HashSet<String> beanNames = shortcut == null ? new HashSet<>() : null;
                DependencyDescriptor descriptor =
                        new DependencyDescriptor(parameter.descriptor()) {
                            @Override
                            public boolean usesStandardBeanLookup() {
                                return false;
                            }

                            @Override
                            public Object resolveShortcut(BeanFactory factory) {
                                return shortcut == null
                                        ? null
                                        : resolveCandidate(shortcut, getDependencyType(), factory);
                            }

                            @Override
                            public Object resolveCandidate(
                                    String beanName, Class<?> requiredType, BeanFactory factory) {
                                Object cached = scopedDependencies.get(beanName);
                                if (cached != null) {
                                    return cached;
                                }
                                Object dependency =
                                        super.resolveCandidate(beanName, requiredType, factory);
                                if (factory.isPrototype(beanName)) {
                                    scopedDependencies.put(beanName, dependency);
                                    ownedDependencies.add(dependency);
                                }
                                return dependency;
                            }
                        };
                Object dependency =
                        beanFactory.resolveDependency(
                                descriptor,
                                constructor.constructor().getDeclaringClass().getName(),
                                beanNames,
                                null);
                if (dependency == null) {
                    throw new IllegalStateException(
                            "Spring dependency is unavailable: " + parameter.typeName());
                }
                arguments[index] = dependency;
                if (shortcut == null) {
                    Optional<String> scalar = shortcutCandidate(parameter, beanNames);
                    if (scalar.isEmpty() && beanNames.stream().anyMatch(beanFactory::isPrototype)) {
                        scopedDependencies.put(parameter.key(), dependency);
                        ownedDependencies.add(dependency);
                    }
                    scalar.filter(
                                    ignored ->
                                            beanFactory
                                                            instanceof
                                                            ConfigurableListableBeanFactory
                                                                    configurable
                                                    && configurable.isConfigurationFrozen())
                            .ifPresent(
                                    candidate ->
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
                preparedShortcut(parameter)
                        .ifPresent(shortcut -> parameter.shortcut().compareAndSet(null, shortcut));
            }
        }
        return plan;
    }

    private Optional<String> preparedShortcut(ParameterPlan parameter) {
        if (!(beanFactory instanceof ConfigurableListableBeanFactory configurable)
                || !configurable.isConfigurationFrozen()) {
            return Optional.empty();
        }
        String selected = null;
        for (String beanName : configurable.getBeanNamesForType(parameter.type(), true, false)) {
            if (!configurable.isAutowireCandidate(beanName, parameter.descriptor())) {
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
        return Optional.of(selected);
    }

    private Optional<String> shortcutCandidate(ParameterPlan parameter, HashSet<String> beanNames) {
        if (beanNames.size() != 1) {
            return Optional.empty();
        }
        String beanName = beanNames.iterator().next();
        if (!beanFactory.containsBean(beanName)
                || !beanFactory.isTypeMatch(beanName, parameter.type())) {
            return Optional.empty();
        }
        return Optional.of(beanName);
    }

    private record HandlerPlan(List<ConstructorPlan> constructors) {
        static HandlerPlan create(Class<?> handlerType) {
            return new HandlerPlan(
                    Arrays.stream(handlerType.getConstructors())
                            .sorted(
                                    Comparator.<Constructor<?>>comparingInt(
                                                    ZLinkSpringHandlerFactory::autowiredPriority)
                                            .thenComparingInt(Constructor::getParameterCount)
                                            .reversed())
                            .map(ConstructorPlan::create)
                            .toList());
        }
    }

    private record ConstructorPlan(Constructor<?> constructor, ParameterPlan[] parameters) {
        static ConstructorPlan create(Constructor<?> constructor) {
            Parameter[] parameters = constructor.getParameters();
            ParameterPlan[] plans = new ParameterPlan[parameters.length];
            for (int index = 0; index < parameters.length; index++) {
                plans[index] =
                        new ParameterPlan(
                                parameters[index].getType(),
                                parameters[index].getParameterizedType().getTypeName(),
                                DependencyKey.from(parameters[index]),
                                new DependencyDescriptor(
                                        new MethodParameter(constructor, index), true),
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
            AtomicReference<String> shortcut) {}

    private static int autowiredPriority(Constructor<?> constructor) {
        return constructor.isAnnotationPresent(Autowired.class) ? 1 : 0;
    }

    private record DependencyKey(String type, List<String> annotations) {
        static DependencyKey from(Parameter parameter) {
            return new DependencyKey(
                    parameter.getParameterizedType().getTypeName(),
                    Arrays.stream(parameter.getDeclaredAnnotations())
                            .map(Object::toString)
                            .sorted()
                            .toList());
        }
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof InvocationTargetException invocation
                        && invocation.getCause() != null
                ? invocation.getCause()
                : failure;
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
