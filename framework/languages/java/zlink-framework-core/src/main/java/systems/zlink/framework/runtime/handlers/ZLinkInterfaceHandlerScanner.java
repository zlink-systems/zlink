package systems.zlink.framework.runtime.handlers;

import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_ENTRY_SPOT_ACTOR_REQUEST_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_ENTRY_SPOT_ACTOR_SEND_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_PUBLISH_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_REQUEST_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_ROUTE_REQUEST_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_ROUTE_SEND_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SEND_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_ACTOR_REQUEST_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_ACTOR_SEND_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_PACKET_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_REQUEST_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_SUBSCRIPTION_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_TIMER_HANDLER;

import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.time.Duration;
import java.util.List;
import java.util.Set;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.handlers.ZLinkSpotTimer;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;

final class ZLinkInterfaceHandlerScanner {
    private ZLinkInterfaceHandlerScanner() {
    }

    static void addHandlers(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups) {
        addChannelInterfaceHandlers(handlers, candidate, groups);
        addSpotInterfaceHandlers(handlers, candidate, groups);
        addSpotActorInterfaceHandlers(handlers, candidate, groups);
    }

    private static void addChannelInterfaceHandlers(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups) {
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(ZLinkSendHandler.class),
            ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.SEND);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(KOTLIN_SEND_HANDLER),
            ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.SEND);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(ZLinkRequestHandler.class),
            ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.REQUEST);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(KOTLIN_REQUEST_HANDLER),
            ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.REQUEST);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(ZLinkFanoutHandler.class),
            ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.PUBLISH);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(KOTLIN_PUBLISH_HANDLER),
            ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.PUBLISH);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(ZLinkRouteSendHandler.class),
            ZLinkScannedHandlerSurface.ROUTE, ZLinkScannedHandlerKind.SEND);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(KOTLIN_ROUTE_SEND_HANDLER),
            ZLinkScannedHandlerSurface.ROUTE, ZLinkScannedHandlerKind.SEND);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(ZLinkRouteRequestHandler.class),
            ZLinkScannedHandlerSurface.ROUTE, ZLinkScannedHandlerKind.REQUEST);
        addInterfaceHandler(handlers, candidate, groups, handlerInterface(KOTLIN_ROUTE_REQUEST_HANDLER),
            ZLinkScannedHandlerSurface.ROUTE, ZLinkScannedHandlerKind.REQUEST);
    }

    private static void addInterfaceHandler(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups,
        HandlerInterfaceMatcher handlerInterface,
        ZLinkScannedHandlerSurface surface,
        ZLinkScannedHandlerKind kind) {
        ParameterizedType matched = handlerInterface.find(candidate);
        if (matched == null) {
            return;
        }
        Type[] arguments = matched.getActualTypeArguments();
        Class<?> messageType = requireClassArgument(candidate, arguments[0]);
        Class<?> replyType = kind == ZLinkScannedHandlerKind.REQUEST
            ? requireClassArgument(candidate, arguments[1])
            : Void.class;
        handlers.add(new ZLinkScannedHandler(
            surface,
            kind,
            candidate,
            messageType,
            replyType,
            resolvePacketName(messageType),
            groups));
    }

    private static void addSpotInterfaceHandlers(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups) {
        addSpotPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(ZLinkSpotPacketHandler.class),
            ZLinkScannedHandlerKind.SEND);
        addSpotPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(KOTLIN_SPOT_PACKET_HANDLER),
            ZLinkScannedHandlerKind.SEND);
        addSpotPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(ZLinkSpotRequestHandler.class),
            ZLinkScannedHandlerKind.REQUEST);
        addSpotPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(KOTLIN_SPOT_REQUEST_HANDLER),
            ZLinkScannedHandlerKind.REQUEST);
        addSpotSubscriptionInterfaceHandler(handlers, candidate, groups);
        addSpotSubscriptionInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(KOTLIN_SPOT_SUBSCRIPTION_HANDLER));
        addSpotTimerInterfaceHandler(handlers, candidate, groups);
        addSpotTimerInterfaceHandler(handlers, candidate, groups, handlerInterface(KOTLIN_SPOT_TIMER_HANDLER));
    }

    private static void addSpotPacketInterfaceHandler(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups,
        HandlerInterfaceMatcher handlerInterface,
        ZLinkScannedHandlerKind kind) {
        ParameterizedType matched = handlerInterface.find(candidate);
        if (matched == null) {
            return;
        }
        Type[] arguments = matched.getActualTypeArguments();
        Class<?> spotType = requireClassArgument(candidate, arguments[0]);
        Class<?> messageType = requireClassArgument(candidate, arguments[1]);
        Class<?> replyType = kind == ZLinkScannedHandlerKind.REQUEST
            ? requireClassArgument(candidate, arguments[2])
            : Void.class;
        handlers.add(new ZLinkScannedHandler(
            ZLinkScannedHandlerSurface.SPOT,
            kind,
            candidate,
            null,
            spotType,
            messageType,
            replyType,
            resolvePacketName(messageType),
            "",
            "",
            null,
            groups));
    }

    private static void addSpotSubscriptionInterfaceHandler(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups) {
        addSpotSubscriptionInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(ZLinkSpotSubscriptionHandler.class));
    }

    private static void addSpotSubscriptionInterfaceHandler(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups,
        HandlerInterfaceMatcher handlerInterface) {
        ParameterizedType matched = handlerInterface.find(candidate);
        if (matched == null) {
            return;
        }
        ZLinkSpotSubscription annotation = candidate.getAnnotation(ZLinkSpotSubscription.class);
        if (annotation == null) {
            return;
        }
        Type[] arguments = matched.getActualTypeArguments();
        Class<?> spotType = requireClassArgument(candidate, arguments[0]);
        Class<?> messageType = requireClassArgument(candidate, arguments[1]);
        handlers.add(new ZLinkScannedHandler(
            ZLinkScannedHandlerSurface.SPOT,
            ZLinkScannedHandlerKind.PUBLISH,
            candidate,
            null,
            spotType,
            messageType,
            Void.class,
            resolvePacketName(messageType),
            ZLinkHandlerScanValidation.requireTopic(candidate, annotation.topic()),
            "",
            null,
            groups));
    }

    private static void addSpotTimerInterfaceHandler(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups) {
        addSpotTimerInterfaceHandler(handlers, candidate, groups, handlerInterface(ZLinkSpotTimerHandler.class));
    }

    private static void addSpotTimerInterfaceHandler(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups,
        HandlerInterfaceMatcher handlerInterface) {
        ParameterizedType matched = handlerInterface.find(candidate);
        if (matched == null) {
            return;
        }
        ZLinkSpotTimer annotation = candidate.getAnnotation(ZLinkSpotTimer.class);
        if (annotation == null) {
            return;
        }
        if (annotation.name().isBlank()) {
            throw new ZLinkConfigurationException(
                "SPOT timer handler name is required: " + candidate.getName());
        }
        if (annotation.periodMillis() <= 0) {
            throw new ZLinkConfigurationException(
                "SPOT timer period must be positive: " + candidate.getName());
        }
        Type[] arguments = matched.getActualTypeArguments();
        Class<?> spotType = requireClassArgument(candidate, arguments[0]);
        handlers.add(new ZLinkScannedHandler(
            ZLinkScannedHandlerSurface.SPOT,
            ZLinkScannedHandlerKind.TIMER,
            candidate,
            null,
            spotType,
            Void.class,
            Void.class,
            "",
            "",
            annotation.name(),
            Duration.ofMillis(annotation.periodMillis()),
            groups));
    }

    private static void addSpotActorInterfaceHandlers(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups) {
        addSpotActorPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(ZLinkEntrySpotActorSendHandler.class),
            ZLinkScannedHandlerKind.ACTOR_SEND);
        addSpotActorPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(KOTLIN_ENTRY_SPOT_ACTOR_SEND_HANDLER),
            ZLinkScannedHandlerKind.ACTOR_SEND);
        addSpotActorPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(ZLinkEntrySpotActorRequestHandler.class),
            ZLinkScannedHandlerKind.ACTOR_REQUEST);
        addSpotActorPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(KOTLIN_ENTRY_SPOT_ACTOR_REQUEST_HANDLER),
            ZLinkScannedHandlerKind.ACTOR_REQUEST);
        addSpotActorPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(ZLinkSpotActorSendHandler.class),
            ZLinkScannedHandlerKind.ACTOR_SEND);
        addSpotActorPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(KOTLIN_SPOT_ACTOR_SEND_HANDLER),
            ZLinkScannedHandlerKind.ACTOR_SEND);
        addSpotActorPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(ZLinkSpotActorRequestHandler.class),
            ZLinkScannedHandlerKind.ACTOR_REQUEST);
        addSpotActorPacketInterfaceHandler(
            handlers,
            candidate,
            groups,
            handlerInterface(KOTLIN_SPOT_ACTOR_REQUEST_HANDLER),
            ZLinkScannedHandlerKind.ACTOR_REQUEST);
    }

    private static void addSpotActorPacketInterfaceHandler(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups,
        HandlerInterfaceMatcher handlerInterface,
        ZLinkScannedHandlerKind kind) {
        ParameterizedType matched = handlerInterface.find(candidate);
        if (matched == null) {
            return;
        }
        Type[] arguments = matched.getActualTypeArguments();
        Class<?> spotType = requireClassArgument(candidate, arguments[0]);
        Class<?> messageType = requireClassArgument(candidate, arguments[2]);
        Class<?> replyType = kind == ZLinkScannedHandlerKind.ACTOR_REQUEST
            ? requireClassArgument(candidate, arguments[3])
            : Void.class;
        handlers.add(new ZLinkScannedHandler(
            ZLinkScannedHandlerSurface.SPOT,
            kind,
            candidate,
            null,
            spotType,
            messageType,
            replyType,
            resolvePacketName(messageType),
            "",
            "",
            null,
            groups));
    }

    private static HandlerInterfaceMatcher handlerInterface(Class<?> type) {
        return candidate -> ZLinkGenericTypeResolver.findInterface(candidate, type);
    }

    private static HandlerInterfaceMatcher handlerInterface(String typeName) {
        return candidate -> ZLinkGenericTypeResolver.findInterface(candidate, typeName);
    }

    private interface HandlerInterfaceMatcher {
        ParameterizedType find(Class<?> candidate);
    }

    private static Class<?> requireClassArgument(Class<?> handlerType, Type argument) {
        return ZLinkGenericTypeResolver.requireClassArgument(handlerType, argument);
    }

    private static String resolvePacketName(Class<?> messageType) {
        return ZLinkPacketNames.resolve(messageType);
    }

}
