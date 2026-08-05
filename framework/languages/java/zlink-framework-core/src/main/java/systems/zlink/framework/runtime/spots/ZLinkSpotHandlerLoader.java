package systems.zlink.framework.runtime.spots;

import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_PACKET_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_REQUEST_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_SUBSCRIPTION_HANDLER;

import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.handlers.ZLinkSpotRequest;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.runtime.handlers.ZLinkGenericTypeResolver;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;

final class ZLinkSpotHandlerLoader {
    private final ZLinkScannedHandlerCatalog scannedHandlers;
    private final ZLinkSpotActorHandlerCatalog actorHandlers;

    ZLinkSpotHandlerLoader(
        ZLinkScannedHandlerCatalog scannedHandlers,
        ZLinkSpotActorHandlerCatalog actorHandlers) {
        this.scannedHandlers = scannedHandlers;
        this.actorHandlers = actorHandlers;
    }

    ZLinkSpotHandlerCatalog.Registrations load(
        Class<?> spotType,
        List<Class<?>> configuredHandlerTypes,
        ScannedTimerRegistrar timerRegistrar) {
        Map<String, SpotPacketHandlerRegistration> packetHandlers = new HashMap<>();
        Map<String, List<SpotSubscriptionHandlerRegistration>> subscriptionHandlers =
            new HashMap<>();
        registerScannedSpotHandlers(
            spotType,
            packetHandlers,
            subscriptionHandlers,
            timerRegistrar);
        for (Class<?> handlerType : configuredHandlerTypes) {
            registerConfiguredSpotHandler(
                handlerType,
                spotType,
                packetHandlers,
                subscriptionHandlers);
        }
        return new ZLinkSpotHandlerCatalog.Registrations(
            packetHandlers,
            subscriptionHandlers);
    }

    private void registerScannedSpotHandlers(
        Class<?> spotType,
        Map<String, SpotPacketHandlerRegistration> packetHandlers,
        Map<String, List<SpotSubscriptionHandlerRegistration>> subscriptionHandlers,
        ScannedTimerRegistrar timerRegistrar) {
        for (ZLinkScannedHandler handler : scannedHandlers.handlers()) {
            if (!isScannedHandlerForSpot(handler, spotType)) {
                continue;
            }
            if (handler.kind() == ZLinkScannedHandlerKind.SEND
                || handler.kind() == ZLinkScannedHandlerKind.REQUEST) {
                addConfiguredPacketHandler(
                    packetHandlers,
                    createScannedSpotPacketRegistration(handler));
                continue;
            }
            if (handler.kind() == ZLinkScannedHandlerKind.PUBLISH) {
                addConfiguredSubscriptionHandler(
                    subscriptionHandlers,
                    createScannedSpotSubscriptionRegistration(handler));
                continue;
            }
            if (handler.kind() == ZLinkScannedHandlerKind.TIMER) {
                timerRegistrar.addTimer(
                    handler.timerName(),
                    handler.timerPeriod(),
                    handler.handlerType(),
                    null);
            }
        }
    }

    private void registerConfiguredSpotHandler(
        Class<?> handlerType,
        Class<?> expectedSpotType,
        Map<String, SpotPacketHandlerRegistration> packetHandlers,
        Map<String, List<SpotSubscriptionHandlerRegistration>> subscriptionHandlers) {
        boolean matched = false;
        if (isSpotPacketHandlerType(handlerType)) {
            addConfiguredPacketHandler(
                packetHandlers,
                createSpotPacketRegistration(handlerType, expectedSpotType));
            matched = true;
        }
        if (isSpotSubscriptionHandlerType(handlerType)) {
            addConfiguredSubscriptionHandler(
                subscriptionHandlers,
                createConfiguredSpotSubscriptionRegistration(handlerType, expectedSpotType));
            matched = true;
        }
        if (actorHandlers.registerConfigured(handlerType, expectedSpotType)) {
            matched = true;
        }
        for (Method method : handlerType.getMethods()) {
            ZLinkSpotSubscription subscription = method.getAnnotation(ZLinkSpotSubscription.class);
            if (subscription != null) {
                addConfiguredSubscriptionHandler(
                    subscriptionHandlers,
                    createSpotSubscriptionRegistration(
                        requireTopic(subscription.topic()),
                        handlerType,
                        expectedSpotType));
                matched = true;
            }
        }
        if (!matched) {
            throw new ZLinkConfigurationException(
                "SPOT handler must declare a SPOT or actor handler contract: "
                    + handlerType.getName());
        }
    }

    private static boolean isScannedHandlerForSpot(
        ZLinkScannedHandler handler,
        Class<?> spotType) {
        return handler.surface() == ZLinkScannedHandlerSurface.SPOT
            && handler.spotType() == spotType
            && (handler.kind() == ZLinkScannedHandlerKind.SEND
                || handler.kind() == ZLinkScannedHandlerKind.REQUEST
                || handler.kind() == ZLinkScannedHandlerKind.PUBLISH
                || handler.kind() == ZLinkScannedHandlerKind.TIMER);
    }

    private static SpotPacketHandlerRegistration createScannedSpotPacketRegistration(
        ZLinkScannedHandler handler) {
        return new SpotPacketHandlerRegistration(
            handler.handlerType(),
            handler.handlerMethod(),
            handler.spotType(),
            handler.messageType(),
            handler.replyType(),
            handler.packetName(),
            handler.kind() == ZLinkScannedHandlerKind.REQUEST);
    }

    private static SpotSubscriptionHandlerRegistration createScannedSpotSubscriptionRegistration(
        ZLinkScannedHandler handler) {
        return new SpotSubscriptionHandlerRegistration(
            handler.topic(),
            handler.handlerType(),
            handler.handlerMethod(),
            handler.spotType(),
            handler.messageType(),
            handler.packetName());
    }

    private static SpotSubscriptionHandlerRegistration createConfiguredSpotSubscriptionRegistration(
        Class<?> handlerType,
        Class<?> expectedSpotType) {
        ZLinkSpotSubscription annotation = handlerType.getAnnotation(ZLinkSpotSubscription.class);
        if (annotation == null) {
            throw new ZLinkConfigurationException(
                "SPOT subscription handler topic is required: " + handlerType.getName());
        }
        return createSpotSubscriptionRegistration(
            requireTopic(annotation.topic()),
            handlerType,
            expectedSpotType);
    }

    private static boolean isSpotPacketHandlerType(Class<?> handlerType) {
        if (findInterface(handlerType, ZLinkSpotPacketHandler.class) != null
            || findInterface(handlerType, ZLinkSpotRequestHandler.class) != null
            || findInterface(handlerType, KOTLIN_SPOT_PACKET_HANDLER) != null
            || findInterface(handlerType, KOTLIN_SPOT_REQUEST_HANDLER) != null) {
            return true;
        }
        for (Method method : handlerType.getMethods()) {
            if (method.getAnnotation(ZLinkSpotRequest.class) != null) {
                return true;
            }
        }
        return false;
    }

    private static boolean isSpotSubscriptionHandlerType(Class<?> handlerType) {
        return findInterface(handlerType, ZLinkSpotSubscriptionHandler.class) != null
            || findInterface(handlerType, KOTLIN_SPOT_SUBSCRIPTION_HANDLER) != null;
    }

    private static void addConfiguredPacketHandler(
        Map<String, SpotPacketHandlerRegistration> handlers,
        SpotPacketHandlerRegistration registration) {
        SpotPacketHandlerRegistration previous =
            handlers.putIfAbsent(registration.packetName(), registration);
        if (previous != null && previous.handlerType() != registration.handlerType()) {
            throw new ZLinkConfigurationException(
                "duplicate SPOT packet handler packet: " + registration.packetName());
        }
    }

    private static void addConfiguredSubscriptionHandler(
        Map<String, List<SpotSubscriptionHandlerRegistration>> handlers,
        SpotSubscriptionHandlerRegistration registration) {
        handlers.computeIfAbsent(registration.topic(), ignored -> new ArrayList<>())
            .add(registration);
    }

    private static SpotPacketHandlerRegistration createSpotPacketRegistration(
        Class<?> handlerType,
        Class<?> expectedSpotType) {
        ParameterizedType packet = findInterface(handlerType, ZLinkSpotPacketHandler.class);
        if (packet == null) {
            packet = findInterface(handlerType, KOTLIN_SPOT_PACKET_HANDLER);
        }
        if (packet != null) {
            Type[] arguments = packet.getActualTypeArguments();
            Class<?> spotType = requireClassArgument(handlerType, arguments[0]);
            requireExactSpotType(handlerType, expectedSpotType, spotType);
            Class<?> messageType = requireClassArgument(handlerType, arguments[1]);
            return new SpotPacketHandlerRegistration(
                handlerType, null, spotType, messageType, Void.class,
                resolvePacketName(messageType), false);
        }

        ParameterizedType request = findInterface(handlerType, ZLinkSpotRequestHandler.class);
        if (request == null) {
            request = findInterface(handlerType, KOTLIN_SPOT_REQUEST_HANDLER);
        }
        if (request != null) {
            Type[] arguments = request.getActualTypeArguments();
            Class<?> spotType = requireClassArgument(handlerType, arguments[0]);
            requireExactSpotType(handlerType, expectedSpotType, spotType);
            Class<?> messageType = requireClassArgument(handlerType, arguments[1]);
            Class<?> replyType = requireClassArgument(handlerType, arguments[2]);
            return new SpotPacketHandlerRegistration(
                handlerType, null, spotType, messageType, replyType,
                resolvePacketName(messageType), true);
        }

        for (Method method : handlerType.getMethods()) {
            ZLinkSpotRequest annotation = method.getAnnotation(ZLinkSpotRequest.class);
            if (annotation == null) {
                continue;
            }
            Class<?>[] parameters = ZLinkHandlerMethodInvoker.logicalParameterTypes(method);
            requireSpotMessageShape(
                handlerType,
                method,
                parameters,
                ZLinkMessageContext.class,
                "SPOT request handler method must have spot and request parameters: ");
            requireExactSpotType(handlerType, expectedSpotType, parameters[0]);
            return new SpotPacketHandlerRegistration(
                handlerType,
                method,
                parameters[0],
                parameters[1],
                resolveReplyType(handlerType, method),
                resolvePacketName(parameters[1], annotation.packetName()),
                true);
        }

        throw new ZLinkConfigurationException(
            "SPOT packet handler must implement ZLinkSpotPacketHandler or ZLinkSpotRequestHandler: "
                + handlerType.getName());
    }

    private static SpotSubscriptionHandlerRegistration createSpotSubscriptionRegistration(
        String topic,
        Class<?> handlerType,
        Class<?> expectedSpotType) {
        ParameterizedType subscription =
            findInterface(handlerType, ZLinkSpotSubscriptionHandler.class);
        if (subscription == null) {
            subscription = findInterface(handlerType, KOTLIN_SPOT_SUBSCRIPTION_HANDLER);
        }
        if (subscription != null) {
            Type[] arguments = subscription.getActualTypeArguments();
            Class<?> spotType = requireClassArgument(handlerType, arguments[0]);
            requireExactSpotType(handlerType, expectedSpotType, spotType);
            Class<?> messageType = requireClassArgument(handlerType, arguments[1]);
            return new SpotSubscriptionHandlerRegistration(
                topic,
                handlerType,
                null,
                spotType,
                messageType,
                resolvePacketName(messageType));
        }

        for (Method method : handlerType.getMethods()) {
            ZLinkSpotSubscription annotation = method.getAnnotation(ZLinkSpotSubscription.class);
            if (annotation == null) {
                continue;
            }
            Class<?>[] parameters = ZLinkHandlerMethodInvoker.logicalParameterTypes(method);
            requireSpotMessageShape(
                handlerType,
                method,
                parameters,
                ZLinkPublishMessageContext.class,
                "SPOT subscription handler method must have spot and event parameters: ");
            requireExactSpotType(handlerType, expectedSpotType, parameters[0]);
            return new SpotSubscriptionHandlerRegistration(
                topic,
                handlerType,
                method,
                parameters[0],
                parameters[1],
                resolvePacketName(parameters[1]));
        }

        throw new ZLinkConfigurationException(
            "SPOT subscription handler must implement ZLinkSpotSubscriptionHandler: "
                + handlerType.getName());
    }

    private static void requireSpotMessageShape(
        Class<?> handlerType,
        Method method,
        Class<?>[] parameters,
        Class<?> contextType,
        String failurePrefix) {
        if (parameters.length == 2) {
            return;
        }
        if (parameters.length == 3
            && parameters[2].isAssignableFrom(contextType)) {
            return;
        }
        throw new ZLinkConfigurationException(
            failurePrefix + handlerType.getName() + "." + method.getName());
    }

    private static void requireExactSpotType(
        Class<?> handlerType,
        Class<?> expectedSpotType,
        Class<?> actualSpotType) {
        if (actualSpotType != expectedSpotType) {
            throw new ZLinkConfigurationException(
                "SPOT handler " + handlerType.getName()
                    + " targets " + actualSpotType.getName()
                    + " but expected " + expectedSpotType.getName());
        }
    }

    private static ParameterizedType findInterface(Class<?> type, Class<?> targetRawType) {
        return ZLinkGenericTypeResolver.findInterface(type, targetRawType);
    }

    private static ParameterizedType findInterface(Class<?> type, String targetRawTypeName) {
        return ZLinkGenericTypeResolver.findInterface(type, targetRawTypeName);
    }

    private static Class<?> requireClassArgument(Class<?> handlerType, Type argument) {
        return ZLinkGenericTypeResolver.requireClassArgument(handlerType, argument);
    }

    private static Class<?> resolveReplyType(Class<?> handlerType, Method method) {
        if (ZLinkHandlerMethodInvoker.isKotlinSuspendMethod(method)) {
            return ZLinkHandlerMethodInvoker.kotlinSuspendReplyType(handlerType, method);
        }
        Type returnType = method.getGenericReturnType();
        if (returnType instanceof ParameterizedType parameterized
            && parameterized.getRawType() == CompletionStage.class) {
            return requireClassArgument(handlerType, parameterized.getActualTypeArguments()[0]);
        }
        if (method.getReturnType() == Void.TYPE || method.getReturnType() == Void.class) {
            throw new ZLinkConfigurationException(
                "SPOT request handler method must return a reply: "
                    + handlerType.getName() + "." + method.getName());
        }
        return method.getReturnType();
    }

    private static String resolvePacketName(Class<?> messageType) {
        return ZLinkPacketNames.resolve(messageType);
    }

    private static String resolvePacketName(Class<?> messageType, String explicitPacketName) {
        return ZLinkPacketNames.resolve(messageType, explicitPacketName);
    }

    private static String requireTopic(String topic) {
        if (topic == null || topic.isBlank()) {
            throw new ZLinkConfigurationException("SPOT subscription topic must not be empty");
        }
        return topic;
    }

    interface ScannedTimerRegistrar {
        CompletionStage<ZLinkTimer> addTimer(
            String name,
            Duration period,
            Class<?> handlerType,
            ZLinkTimerOptions options);
    }
}
