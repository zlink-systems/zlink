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
import org.springframework.beans.factory.support.AbstractBeanDefinition;
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
import systems.zlink.framework.runtime.internal.handlers.ZLinkTypeAwareHandlerActivator;
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
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

final class ZLinkSpringHandlerFactory
        implements ZLinkHandlerActivator, ZLinkTypeAwareHandlerActivator {
    private static final String CLOSE_METHOD_NAME = "close";
    private static final String PRE_DESTROY_ANNOTATION_CLASS_NAME = "jakarta.annotation.PreDestroy";
    private static final String ACTIVATION_BEAN_NAME_SUFFIX = "#zlinkActivation";
    private static final ClassValue<HandlerMetadata> HANDLER_METADATA =
            new ClassValue<>() {
                @Override
                protected HandlerMetadata computeValue(Class<?> type) {
                    return new HandlerMetadata(
                            Arrays.stream(type.getConstructors())
                                    .sorted(
                                            Comparator.<Constructor<?>>comparingInt(
                                                            ZLinkSpringHandlerFactory
                                                                    ::autowiredPriority)
                                                    .thenComparingInt(
                                                            Constructor::getParameterCount)
                                                    .reversed())
                                    .toList(),
                            type.getName() + ACTIVATION_BEAN_NAME_SUFFIX,
                            springInvokesClose(type));
                }
            };
    private final AutowireCapableBeanFactory beanFactory;
    private final Map<Class<?>, HandlerPlan> handlerPlans = new ConcurrentHashMap<>();

    ZLinkSpringHandlerFactory(AutowireCapableBeanFactory beanFactory) {
        this.beanFactory = Objects.requireNonNull(beanFactory, "beanFactory");
    }

    @Override
    public void prepare(Class<?> handlerType) {
        handlerPlan(handlerType);
    }

    @Override
    public PreparedConstruction prepareConstruction(Class<?> type, Set<Class<?>> runtimeServices) {
        HandlerConstruction plan =
                createHandlerConstruction(handlerRegistration(type), runtimeServices);
        return (activation, resolver) -> activation.createPrepared(plan, resolver);
    }

    @Override
    public Activation reusableActivation(
            Class<?> handlerType,
            ZLinkTypeAwareHandlerActivator.RuntimeServiceLookup runtimeServices) {
        HandlerPlan plan = handlerPlans.get(handlerType);
        if (plan == null || plan.singletonBeanName == null) {
            return null;
        }
        for (Class<?> dependencyType : plan.constructorDependencies) {
            if (runtimeServices.contains(dependencyType)) {
                return null;
            }
        }
        return plan.reusableActivation;
    }

    private String validateDependency(DependencyDescriptor parameter) {
        Class<?> type = parameter.getDependencyType();
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
            return null;
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
            return null;
        }
        String selected =
                candidates.size() == 1
                        ? candidates.keySet().iterator().next()
                        : new MetadataCandidateSelector(source).select(candidates, descriptor);
        if (selected == null) {
            throw new NoUniqueBeanDefinitionException(
                    descriptor.getResolvableType(), candidates.keySet());
        }
        return type == Optional.class ? null : selected;
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
        return new SpringActivation().create(handlerType);
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
        boolean springInvokesClose = HANDLER_METADATA.get(instance.getClass()).springInvokesClose();
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

    /**
     * One dispatch scope. Its owner ({@code ZLinkHandlerInstanceOwner}) serializes every call on
     * its state lane, so the scope holds no lock of its own. Scoped dependencies are recorded only
     * when a prototype dependency is created.
     */
    private final class SpringActivation implements Activation {
        private Map<Object, Object> scopedDependencies;
        private List<Object> ownedDependencies;
        private boolean closed;

        @Override
        public Object create(Class<?> handlerType) {
            return create(handlerType, ignored -> null);
        }

        @Override
        public Object create(Class<?> handlerType, DependencyResolver dependencyResolver) {
            return create(handlerPlan(handlerType), dependencyResolver);
        }

        @Override
        public Object createPrepared(Object construction, DependencyResolver resolver) {
            if (construction instanceof HandlerConstruction plan) {
                return create(plan, resolver);
            }
            return Activation.super.createPrepared(construction, resolver);
        }

        private Object create(HandlerConstruction plan, DependencyResolver dependencyResolver) {
            if (closed) {
                throw new IllegalStateException("Spring handler activation is closed");
            }
            ConstructorPlan constructor = plan.constructor();
            Object[] arguments = resolveArguments(constructor, dependencyResolver);
            return constructHandler(plan, arguments);
        }

        private Object[] resolveArguments(
                ConstructorPlan constructor, DependencyResolver dependencyResolver) {
            ParameterPlan[] parameters = constructor.parameters();
            Object[] arguments = parameters.length == 0 ? null : new Object[parameters.length];
            for (int index = 0; index < parameters.length; index++) {
                ParameterPlan parameter = parameters[index];
                if (parameter.descriptor() == null) {
                    arguments[index] = dependencyResolver.resolve(parameter.type());
                    if (arguments[index] == null) {
                        throw new IllegalStateException(
                                "Runtime dependency is unavailable: " + parameter.typeName());
                    }
                    continue;
                }
                Object aggregate = scopedDependency(parameter.key());
                if (aggregate != null) {
                    arguments[index] = aggregate;
                    continue;
                }
                String shortcut = parameter.shortcut();
                Object singleton = createdSingleton(shortcut);
                if (singleton != null) {
                    arguments[index] = singleton;
                    continue;
                }
                int ownedBefore = ownedDependencies == null ? 0 : ownedDependencies.size();
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
                                Object cached = scopedDependency(beanName);
                                if (cached != null) {
                                    return cached;
                                }
                                Object dependency =
                                        super.resolveCandidate(beanName, requiredType, factory);
                                if (factory.isPrototype(beanName)) {
                                    own(beanName, dependency);
                                }
                                return dependency;
                            }
                        };
                Object dependency =
                        beanFactory.resolveDependency(
                                descriptor,
                                constructor.constructor().getDeclaringClass().getName(),
                                null,
                                null);
                if (dependency == null && parameter.descriptor().isRequired()) {
                    throw new IllegalStateException(
                            "Spring dependency is unavailable: " + parameter.typeName());
                }
                arguments[index] = dependency;
                if (shortcut == null
                        && ownedDependencies != null
                        && ownedDependencies.size() > ownedBefore) {
                    scopedDependencies.put(parameter.key(), dependency);
                }
            }
            return arguments;
        }

        /** A prepared singleton shortcut resolves directly by bean name. */
        private Object createdSingleton(String shortcut) {
            return shortcut != null
                            && beanFactory instanceof ConfigurableListableBeanFactory configurable
                            && configurable.containsSingleton(shortcut)
                    ? configurable.getBean(shortcut)
                    : null;
        }

        private Object scopedDependency(Object key) {
            return scopedDependencies == null ? null : scopedDependencies.get(key);
        }

        private void own(Object key, Object dependency) {
            if (scopedDependencies == null) {
                scopedDependencies = new LinkedHashMap<>();
                ownedDependencies = new ArrayList<>();
            }
            scopedDependencies.put(key, dependency);
            ownedDependencies.add(dependency);
        }

        @Override
        public void destroy(Object instance) {
            destroyOwned(instance);
        }

        @Override
        public void close() {
            if (closed) {
                return;
            }
            closed = true;
            List<Object> dependencies = ownedDependencies == null ? List.of() : ownedDependencies;
            ownedDependencies = null;
            scopedDependencies = null;
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

    private final class SharedSpringActivation implements Activation {
        private final HandlerConstruction construction;

        private SharedSpringActivation(HandlerConstruction construction) {
            this.construction = construction;
        }

        @Override
        public Object create(Class<?> handlerType) {
            if (handlerType != construction.registration().handlerType) {
                throw new IllegalArgumentException(
                        "shared handler activation belongs to "
                                + construction.registration().handlerType.getName());
            }
            ConstructorPlan constructor = construction.constructor();
            ParameterPlan[] parameters = constructor.parameters();
            Object[] arguments = parameters.length == 0 ? null : new Object[parameters.length];
            for (int index = 0; index < parameters.length; index++) {
                ParameterPlan parameter = parameters[index];
                if (parameter.descriptor() == null || parameter.shortcut() == null) {
                    throw new IllegalStateException(
                            "shared handler construction has a scoped dependency");
                }
                arguments[index] = beanFactory.getBean(parameter.shortcut());
            }
            return constructHandler(construction, arguments);
        }

        @Override
        public void destroy(Object instance) {
            destroyOwned(instance);
        }

        @Override
        public void close() {}
    }

    private Object constructHandler(HandlerConstruction construction, Object[] arguments) {
        Class<?> handlerType = construction.registration().handlerType;
        try {
            Object instance = construction.constructor().constructor().newInstance(arguments);
            beanFactory.autowireBean(instance);
            return beanFactory.initializeBean(
                    instance, construction.registration().metadata.beanName());
        } catch (ReflectiveOperationException failure) {
            throw new IllegalStateException(
                    "failed to construct Framework-owned handler: " + handlerType.getName(),
                    unwrap(failure));
        }
    }

    private HandlerConstruction handlerPlan(Class<?> handlerType) {
        HandlerPlan registration = handlerRegistration(handlerType);
        HandlerConstruction prepared = registration.defaultConstruction;
        if (prepared != null) {
            return prepared;
        }
        synchronized (registration) {
            prepared = registration.defaultConstruction;
            if (prepared == null) {
                prepared = createHandlerConstruction(registration, Set.of());
                registration.defaultConstruction = prepared;
            }
        }
        return prepared;
    }

    private HandlerPlan handlerRegistration(Class<?> handlerType) {
        HandlerPlan prepared = handlerPlans.get(handlerType);
        if (prepared != null) {
            return prepared;
        }
        return handlerPlans.computeIfAbsent(handlerType, this::createHandlerPlan);
    }

    private HandlerPlan createHandlerPlan(Class<?> handlerType) {
        HandlerMetadata metadata = HANDLER_METADATA.get(handlerType);
        SingletonHandlerRegistration singleton = singletonHandlerRegistration(handlerType);
        return new HandlerPlan(
                handlerType, metadata, singleton.beanName(), singleton.constructorDependencies());
    }

    private SingletonHandlerRegistration singletonHandlerRegistration(Class<?> handlerType) {
        if (!isZLinkManagedType(handlerType)
                || handlerType.isInterface()
                || java.lang.reflect.Modifier.isAbstract(handlerType.getModifiers())
                || !(beanFactory instanceof ConfigurableListableBeanFactory configurable)) {
            return SingletonHandlerRegistration.NOT_REUSABLE;
        }
        String[] candidates =
                BeanFactoryUtils.beanNamesForTypeIncludingAncestors(
                        configurable, handlerType, true, false);
        if (candidates.length != 1) {
            return SingletonHandlerRegistration.NOT_REUSABLE;
        }
        String beanName = candidates[0];
        if (!configurable.containsBeanDefinition(beanName) || !configurable.isSingleton(beanName)) {
            return SingletonHandlerRegistration.NOT_REUSABLE;
        }
        var definition = configurable.getBeanDefinition(beanName);
        if (!(definition instanceof AbstractBeanDefinition abstractDefinition)) {
            return SingletonHandlerRegistration.NOT_REUSABLE;
        }
        boolean factoryMethod = definition.getFactoryMethodName() != null;
        boolean supplier = abstractDefinition.getInstanceSupplier() != null;
        boolean directHandler =
                abstractDefinition.hasBeanClass()
                        && abstractDefinition.getBeanClass() == handlerType;
        if (!factoryMethod && !supplier && !directHandler) {
            return SingletonHandlerRegistration.NOT_REUSABLE;
        }
        Class<?> candidateType = configurable.getType(beanName, false);
        if (candidateType == null || !handlerType.isAssignableFrom(candidateType)) {
            return SingletonHandlerRegistration.NOT_REUSABLE;
        }
        if (configurable.containsSingleton(beanName)
                && !handlerType.isInstance(configurable.getBean(beanName))) {
            return SingletonHandlerRegistration.NOT_REUSABLE;
        }
        List<Constructor<?>> constructors = HANDLER_METADATA.get(handlerType).constructors();
        if (constructors.isEmpty()) {
            return SingletonHandlerRegistration.NOT_REUSABLE;
        }
        List<Class<?>> constructorDependencies =
                constructors.stream()
                        .flatMap(constructor -> Arrays.stream(constructor.getParameterTypes()))
                        .distinct()
                        .toList();
        return new SingletonHandlerRegistration(beanName, constructorDependencies);
    }

    private HandlerConstruction createHandlerConstruction(
            HandlerPlan registration, Set<Class<?>> runtimeServices) {
        RuntimeException failure = null;
        HandlerMetadata metadata = registration.metadata;
        for (Constructor<?> constructor : metadata.constructors()) {
            try {
                Parameter[] parameters = constructor.getParameters();
                ParameterPlan[] plans = new ParameterPlan[parameters.length];
                for (int index = 0; index < parameters.length; index++) {
                    Parameter parameter = parameters[index];
                    DependencyDescriptor descriptor =
                            new DependencyDescriptor(new MethodParameter(constructor, index), true);
                    String shortcut = null;
                    if (runtimeServices.contains(parameter.getType())) {
                        descriptor = null;
                    } else {
                        shortcut = validateDependency(descriptor);
                    }
                    plans[index] =
                            new ParameterPlan(
                                    parameter.getType(),
                                    parameter.getParameterizedType().getTypeName(),
                                    DependencyKey.from(parameter),
                                    descriptor,
                                    shortcut);
                }
                var constructorPlan = new ConstructorPlan(constructor, plans);
                var construction = new HandlerConstruction(constructorPlan, registration);
                if (registration.singletonBeanName != null
                        && hasOnlySingletonConstructorDependencies(constructorPlan)
                        && registration.reusableActivation == null) {
                    registration.reusableActivation = new SharedSpringActivation(construction);
                }
                return construction;
            } catch (NoSuchBeanDefinitionException unavailable) {
                failure = unavailable;
            }
        }
        if (failure != null) {
            throw failure;
        }
        throw new IllegalStateException(
                "No public constructor: " + registration.handlerType.getName());
    }

    private boolean hasOnlySingletonConstructorDependencies(ConstructorPlan constructor) {
        for (ParameterPlan parameter : constructor.parameters()) {
            if (parameter.descriptor() == null
                    || parameter.shortcut() == null
                    || isLazyDependency(parameter.descriptor(), parameter.shortcut())
                    || !isKnownSingletonDependency(parameter.shortcut(), parameter.type())) {
                return false;
            }
        }
        return true;
    }

    private boolean isLazyDependency(DependencyDescriptor descriptor, String beanName) {
        if (!(beanFactory instanceof DefaultListableBeanFactory source)) {
            return false;
        }
        return source.getAutowireCandidateResolver()
                        .getLazyResolutionProxyClass(descriptor, beanName)
                != null;
    }

    private boolean isKnownSingletonDependency(String beanName, Class<?> dependencyType) {
        if (!(beanFactory instanceof ConfigurableListableBeanFactory configurable)
                || !configurable.isSingleton(beanName)
                || !configurable.containsBeanDefinition(beanName)) {
            return false;
        }
        var definition = configurable.getBeanDefinition(beanName);
        if (!(definition instanceof AbstractBeanDefinition abstractDefinition)) {
            return false;
        }
        boolean factoryMethod = definition.getFactoryMethodName() != null;
        boolean supplier = abstractDefinition.getInstanceSupplier() != null;
        boolean directDependency =
                abstractDefinition.hasBeanClass()
                        && dependencyType.isAssignableFrom(abstractDefinition.getBeanClass());
        if (!factoryMethod && !supplier && !directDependency) {
            return false;
        }
        Class<?> candidateType = configurable.getType(beanName, false);
        return candidateType != null && dependencyType.isAssignableFrom(candidateType);
    }

    private record HandlerMetadata(
            List<Constructor<?>> constructors, String beanName, boolean springInvokesClose) {}

    private static final class HandlerPlan {
        private final Class<?> handlerType;
        private final HandlerMetadata metadata;
        private final String singletonBeanName;
        private final List<Class<?>> constructorDependencies;
        private volatile HandlerConstruction defaultConstruction;
        private volatile Activation reusableActivation;

        private HandlerPlan(
                Class<?> handlerType,
                HandlerMetadata metadata,
                String singletonBeanName,
                List<Class<?>> constructorDependencies) {
            this.handlerType = handlerType;
            this.metadata = metadata;
            this.singletonBeanName = singletonBeanName;
            this.constructorDependencies = constructorDependencies;
        }
    }

    private record SingletonHandlerRegistration(
            String beanName, List<Class<?>> constructorDependencies) {
        private static final SingletonHandlerRegistration NOT_REUSABLE =
                new SingletonHandlerRegistration(null, List.of());
    }

    private record HandlerConstruction(ConstructorPlan constructor, HandlerPlan registration) {}

    private record ConstructorPlan(Constructor<?> constructor, ParameterPlan[] parameters) {}

    private record ParameterPlan(
            Class<?> type,
            String typeName,
            DependencyKey key,
            DependencyDescriptor descriptor,
            String shortcut) {}

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
