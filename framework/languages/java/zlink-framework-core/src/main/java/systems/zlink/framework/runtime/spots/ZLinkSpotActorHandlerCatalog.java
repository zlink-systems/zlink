package systems.zlink.framework.runtime.spots;

import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_ENTRY_SPOT_ACTOR_REQUEST_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_ENTRY_SPOT_ACTOR_SEND_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_ACTOR_REQUEST_HANDLER;
import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SPOT_ACTOR_SEND_HANDLER;

import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.runtime.handlers.ZLinkGenericTypeResolver;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;

final class ZLinkSpotActorHandlerCatalog {
    private final ZLinkMessageSerializer serializer;
    private final Map<String, List<SpotActorPacketHandlerRegistration>> handlersByPacket =
        new HashMap<>();

    ZLinkSpotActorHandlerCatalog(
        ZLinkScannedHandlerCatalog scannedHandlers,
        ZLinkMessageSerializer serializer) {
        this.serializer = serializer;
        for (ZLinkScannedHandler handler : scannedHandlers.handlers()) {
            if (handler.surface() != ZLinkScannedHandlerSurface.SPOT
                || (handler.kind() != ZLinkScannedHandlerKind.ACTOR_SEND
                    && handler.kind() != ZLinkScannedHandlerKind.ACTOR_REQUEST)) {
                continue;
            }
            add(createScannedRegistration(handler));
        }
    }

    List<SpotActorPacketHandlerRegistration> handlers(String packetName) {
        return handlersByPacket.get(packetName);
    }

    boolean registerConfigured(Class<?> handlerType, Class<?> expectedSpotType) {
        boolean matched = false;
        if (isActorPacketHandlerType(handlerType)) {
            registerInterfaceHandler(handlerType);
            matched = true;
        }
        for (Method method : handlerType.getMethods()) {
            rejectConflictingAnnotations(handlerType, method);
            if (method.getAnnotation(ZLinkSpotActorSend.class) != null) {
                registerAnnotatedHandler(
                    handlerType,
                    expectedSpotType,
                    method,
                    ZLinkScannedHandlerKind.ACTOR_SEND);
                matched = true;
            }
            if (method.getAnnotation(ZLinkSpotActorRequest.class) != null) {
                registerAnnotatedHandler(
                    handlerType,
                    expectedSpotType,
                    method,
                    ZLinkScannedHandlerKind.ACTOR_REQUEST);
                matched = true;
            }
        }
        return matched;
    }

    private void registerAnnotatedHandler(
        Class<?> handlerType,
        Class<?> expectedSpotType,
        Method method,
        ZLinkScannedHandlerKind kind) {
        ActorMessageShape shape = actorPacketHandlerShape(
            handlerType,
            method,
            kind == ZLinkScannedHandlerKind.ACTOR_REQUEST
                ? ZLinkMessageContext.class
                : ZLinkMessageContext.class);
        Class<?> replyType = kind == ZLinkScannedHandlerKind.ACTOR_REQUEST
            ? resolveReplyType(handlerType, method)
            : Void.class;
        String explicitPacketName = kind == ZLinkScannedHandlerKind.ACTOR_REQUEST
            ? method.getAnnotation(ZLinkSpotActorRequest.class).packetName()
            : method.getAnnotation(ZLinkSpotActorSend.class).packetName();
        add(new SpotActorPacketHandlerRegistration(
            handlerType,
            method,
            expectedSpotType,
            shape.actorType(),
            shape.messageType(),
            replyType,
            ZLinkPacketNames.resolve(shape.messageType(), explicitPacketName),
            kind));
    }

    private void registerInterfaceHandler(Class<?> handlerType) {
        registerInterfaceHandler(
            handlerType,
            ZLinkScannedHandlerKind.ACTOR_SEND,
            findInterface(handlerType, ZLinkEntrySpotActorSendHandler.class),
            findInterface(handlerType, ZLinkSpotActorSendHandler.class));
        registerInterfaceHandler(
            handlerType,
            ZLinkScannedHandlerKind.ACTOR_SEND,
            findInterface(handlerType, KOTLIN_ENTRY_SPOT_ACTOR_SEND_HANDLER),
            findInterface(handlerType, KOTLIN_SPOT_ACTOR_SEND_HANDLER));
        registerInterfaceHandler(
            handlerType,
            ZLinkScannedHandlerKind.ACTOR_REQUEST,
            findInterface(handlerType, ZLinkEntrySpotActorRequestHandler.class),
            findInterface(handlerType, ZLinkSpotActorRequestHandler.class));
        registerInterfaceHandler(
            handlerType,
            ZLinkScannedHandlerKind.ACTOR_REQUEST,
            findInterface(handlerType, KOTLIN_ENTRY_SPOT_ACTOR_REQUEST_HANDLER),
            findInterface(handlerType, KOTLIN_SPOT_ACTOR_REQUEST_HANDLER));
    }

    private void registerInterfaceHandler(
        Class<?> handlerType,
        ZLinkScannedHandlerKind kind,
        ParameterizedType entryInterface,
        ParameterizedType spotInterface) {
        ParameterizedType matched = entryInterface != null ? entryInterface : spotInterface;
        if (matched == null) {
            return;
        }
        Type[] arguments = matched.getActualTypeArguments();
        Class<?> messageType = requireClassArgument(handlerType, arguments[2]);
        add(new SpotActorPacketHandlerRegistration(
            handlerType,
            null,
            requireClassArgument(handlerType, arguments[0]),
            requireClassArgument(handlerType, arguments[1]),
            messageType,
            kind == ZLinkScannedHandlerKind.ACTOR_REQUEST
                ? requireClassArgument(handlerType, arguments[3])
                : Void.class,
            ZLinkPacketNames.resolve(messageType),
            kind));
    }

    private void add(SpotActorPacketHandlerRegistration registration) {
        List<SpotActorPacketHandlerRegistration> packetHandlers =
            handlersByPacket.computeIfAbsent(
                registration.packetName(),
                ignored -> new ArrayList<>());
        for (SpotActorPacketHandlerRegistration existing : packetHandlers) {
            if (existing.spotType() == registration.spotType()
                && existing.actorType() == registration.actorType()
                && existing.kind() == registration.kind()) {
                if (existing.handlerType() == registration.handlerType()) {
                    return;
                }
                throw new ZLinkConfigurationException(
                    "duplicate Spot actor packet handler packet: " + registration.packetName());
            }
        }
        packetHandlers.add(registration);
        serializer.prepare(registration.messageType());
        serializer.prepare(registration.replyType());
    }

    private static SpotActorPacketHandlerRegistration createScannedRegistration(
        ZLinkScannedHandler handler) {
        if (handler.handlerMethod() != null) {
            ActorMessageShape shape = actorPacketHandlerShape(
                handler.handlerType(),
                handler.handlerMethod(),
                handler.kind() == ZLinkScannedHandlerKind.ACTOR_REQUEST
                    ? ZLinkMessageContext.class
                    : ZLinkMessageContext.class);
            return new SpotActorPacketHandlerRegistration(
                handler.handlerType(),
                handler.handlerMethod(),
                handler.spotType() != null ? handler.spotType() : shape.spotType(),
                shape.actorType(),
                handler.messageType(),
                handler.replyType(),
                handler.packetName(),
                handler.kind());
        }
        ParameterizedType matched = findActorPacketInterface(
            handler.handlerType(),
            handler.kind());
        if (matched == null) {
            throw new ZLinkConfigurationException(
                "Spot actor packet interface handler does not match its scanned kind: "
                    + handler.handlerType().getName());
        }
        Type[] arguments = matched.getActualTypeArguments();
        return new SpotActorPacketHandlerRegistration(
            handler.handlerType(),
            null,
            requireClassArgument(handler.handlerType(), arguments[0]),
            requireClassArgument(handler.handlerType(), arguments[1]),
            handler.messageType(),
            handler.replyType(),
            handler.packetName(),
            handler.kind());
    }

    private static ParameterizedType findActorPacketInterface(
        Class<?> handlerType,
        ZLinkScannedHandlerKind kind) {
        if (kind == ZLinkScannedHandlerKind.ACTOR_REQUEST) {
            ParameterizedType entry =
                findInterface(handlerType, ZLinkEntrySpotActorRequestHandler.class);
            if (entry != null) {
                return entry;
            }
            ParameterizedType spot =
                findInterface(handlerType, ZLinkSpotActorRequestHandler.class);
            if (spot != null) {
                return spot;
            }
            entry = findInterface(handlerType, KOTLIN_ENTRY_SPOT_ACTOR_REQUEST_HANDLER);
            return entry != null
                ? entry
                : findInterface(handlerType, KOTLIN_SPOT_ACTOR_REQUEST_HANDLER);
        }
        ParameterizedType entry =
            findInterface(handlerType, ZLinkEntrySpotActorSendHandler.class);
        if (entry != null) {
            return entry;
        }
        ParameterizedType spot = findInterface(handlerType, ZLinkSpotActorSendHandler.class);
        if (spot != null) {
            return spot;
        }
        entry = findInterface(handlerType, KOTLIN_ENTRY_SPOT_ACTOR_SEND_HANDLER);
        return entry != null
            ? entry
            : findInterface(handlerType, KOTLIN_SPOT_ACTOR_SEND_HANDLER);
    }

    private static boolean isActorPacketHandlerType(Class<?> handlerType) {
        return findInterface(handlerType, ZLinkEntrySpotActorSendHandler.class) != null
            || findInterface(handlerType, ZLinkEntrySpotActorRequestHandler.class) != null
            || findInterface(handlerType, ZLinkSpotActorSendHandler.class) != null
            || findInterface(handlerType, ZLinkSpotActorRequestHandler.class) != null
            || findInterface(handlerType, KOTLIN_ENTRY_SPOT_ACTOR_SEND_HANDLER) != null
            || findInterface(handlerType, KOTLIN_ENTRY_SPOT_ACTOR_REQUEST_HANDLER) != null
            || findInterface(handlerType, KOTLIN_SPOT_ACTOR_SEND_HANDLER) != null
            || findInterface(handlerType, KOTLIN_SPOT_ACTOR_REQUEST_HANDLER) != null;
    }

    private static void rejectConflictingAnnotations(Class<?> handlerType, Method method) {
        if (method.getAnnotation(ZLinkSpotActorSend.class) != null
            && method.getAnnotation(ZLinkSpotActorRequest.class) != null) {
            throw new ZLinkConfigurationException(
                "SPOT actor handler method cannot declare both send and request annotations: "
                    + handlerType.getName() + "." + method.getName());
        }
    }

    private static ActorMessageShape actorPacketHandlerShape(
        Class<?> handlerType,
        Method method,
        Class<?> contextType) {
        Class<?>[] parameters = ZLinkHandlerMethodInvoker.logicalParameterTypes(method);
        if (parameters.length == 2) {
            return new ActorMessageShape(null, parameters[0], parameters[1]);
        }
        if (parameters.length == 4
            && parameters[2].isAssignableFrom(contextType)) {
            return new ActorMessageShape(parameters[0], parameters[1], parameters[3]);
        }
        throw new ZLinkConfigurationException(
            "Spot actor packet handler method must have actor/message or spot, actor, context, message parameters: "
                + handlerType.getName() + "." + method.getName());
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

    private record ActorMessageShape(Class<?> spotType, Class<?> actorType, Class<?> messageType) {
    }
}
