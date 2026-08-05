package systems.zlink.framework.spring;

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
import org.springframework.beans.BeansException;
import org.springframework.beans.factory.DisposableBean;
import org.springframework.beans.factory.config.AutowireCapableBeanFactory;
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

    ZLinkSpringHandlerFactory(AutowireCapableBeanFactory beanFactory) {
        this.beanFactory = beanFactory;
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
            Constructor<?>[] constructors = handlerType.getConstructors();
            java.util.Arrays.sort(
                constructors,
                Comparator.<Constructor<?>>comparingInt(
                        ZLinkSpringHandlerFactory::autowiredPriority)
                    .thenComparingInt(Constructor::getParameterCount)
                    .reversed());
            for (Constructor<?> constructor : constructors) {
                try {
                    Object[] arguments = resolveArguments(
                        constructor,
                        dependencyResolver);
                    Object instance = constructor.newInstance(arguments);
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
            Constructor<?> constructor,
            DependencyResolver dependencyResolver) {
            Parameter[] parameters = constructor.getParameters();
            Object[] arguments = new Object[parameters.length];
            for (int index = 0; index < parameters.length; index++) {
                Object supplied = dependencyResolver.resolve(
                    parameters[index].getType());
                if (supplied != null) {
                    arguments[index] = supplied;
                    continue;
                }
                DependencyKey key = DependencyKey.from(parameters[index]);
                Object cached = scopedDependencies.get(key);
                if (cached != null) {
                    arguments[index] = cached;
                    continue;
                }
                HashSet<String> beanNames = new HashSet<>();
                DependencyDescriptor descriptor = new DependencyDescriptor(
                    new MethodParameter(constructor, index),
                    true);
                Object dependency = beanFactory.resolveDependency(
                    descriptor,
                    constructor.getDeclaringClass().getName(),
                    beanNames,
                    null);
                if (dependency == null) {
                    throw new IllegalStateException(
                        "Spring dependency is unavailable: "
                            + parameters[index].getParameterizedType().getTypeName());
                }
                boolean activationScoped = !beanNames.isEmpty()
                    && beanNames.stream().anyMatch(beanFactory::isPrototype);
                if (activationScoped) {
                    scopedDependencies.put(key, dependency);
                    ownedDependencies.add(dependency);
                }
                arguments[index] = dependency;
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
                java.util.Arrays.stream(parameter.getDeclaredAnnotations())
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
