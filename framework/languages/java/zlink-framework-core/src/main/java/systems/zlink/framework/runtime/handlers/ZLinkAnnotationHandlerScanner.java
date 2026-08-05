package systems.zlink.framework.runtime.handlers;

import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.util.List;
import java.util.Set;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.handlers.ZLinkPublish;
import systems.zlink.framework.handlers.ZLinkRequest;
import systems.zlink.framework.handlers.ZLinkSend;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.handlers.ZLinkSpotRequest;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;

final class ZLinkAnnotationHandlerScanner {
    private ZLinkAnnotationHandlerScanner() {
    }

    static void addHandlers(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Set<String> groups) {
        for (Method method : candidate.getMethods()) {
            rejectConflictingSpotActorAnnotations(candidate, method);
            addChannelHandlers(handlers, candidate, method, groups);
            addSpotHandlers(handlers, candidate, method, groups);
            addSpotActorHandlers(handlers, candidate, method, groups);
        }
    }

    private static void addChannelHandlers(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Method method,
        Set<String> groups) {
        ZLinkSend send = method.getAnnotation(ZLinkSend.class);
        if (send != null) {
            requireJavaCompletionStageReturn(candidate, method);
            Class<?> messageType = requireChannelHandlerShape(
                candidate, method, ZLinkMessageContext.class);
            handlers.add(new ZLinkScannedHandler(
                ZLinkScannedHandlerSurface.CHANNEL,
                ZLinkScannedHandlerKind.SEND,
                candidate,
                method,
                messageType,
                Void.class,
                resolvePacketName(messageType, send.packetName()),
                groups));
        }

        ZLinkRequest request = method.getAnnotation(ZLinkRequest.class);
        if (request != null) {
            requireJavaCompletionStageReturn(candidate, method);
            Class<?> messageType = requireChannelHandlerShape(
                candidate, method, ZLinkMessageContext.class);
            Class<?> replyType = resolveReplyType(candidate, method);
            handlers.add(new ZLinkScannedHandler(
                ZLinkScannedHandlerSurface.CHANNEL,
                ZLinkScannedHandlerKind.REQUEST,
                candidate,
                method,
                messageType,
                replyType,
                resolvePacketName(messageType, request.packetName()),
                groups));
        }

        ZLinkPublish publish = method.getAnnotation(ZLinkPublish.class);
        if (publish != null) {
            requireJavaCompletionStageReturn(candidate, method);
            Class<?> messageType = requireChannelHandlerShape(
                candidate, method, ZLinkPublishMessageContext.class);
            handlers.add(new ZLinkScannedHandler(
                ZLinkScannedHandlerSurface.CHANNEL,
                ZLinkScannedHandlerKind.PUBLISH,
                candidate,
                method,
                messageType,
                Void.class,
                resolvePacketName(messageType, publish.packetName()),
                groups));
        }
    }

    private static void addSpotHandlers(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Method method,
        Set<String> groups) {
        ZLinkSpotRequest spotRequest = method.getAnnotation(ZLinkSpotRequest.class);
        if (spotRequest != null) {
            requireJavaCompletionStageReturn(candidate, method);
            SpotMethodShape shape = requireSpotMethodShape(
                candidate,
                method,
                ZLinkMessageContext.class,
                "SPOT request handler method must have spot and request parameters: ");
            Class<?> replyType = resolveReplyType(candidate, method);
            handlers.add(new ZLinkScannedHandler(
                ZLinkScannedHandlerSurface.SPOT,
                ZLinkScannedHandlerKind.REQUEST,
                candidate,
                method,
                shape.spotType(),
                shape.messageType(),
                replyType,
                resolvePacketName(shape.messageType(), spotRequest.packetName()),
                "",
                "",
                null,
                groups));
        }

        ZLinkSpotSubscription spotSubscription = method.getAnnotation(ZLinkSpotSubscription.class);
        if (spotSubscription != null) {
            requireJavaCompletionStageReturn(candidate, method);
            SpotMethodShape shape = requireSpotMethodShape(
                candidate,
                method,
                ZLinkPublishMessageContext.class,
                "SPOT subscription handler method must have spot and event parameters: ");
            handlers.add(new ZLinkScannedHandler(
                ZLinkScannedHandlerSurface.SPOT,
                ZLinkScannedHandlerKind.PUBLISH,
                candidate,
                method,
                shape.spotType(),
                shape.messageType(),
                Void.class,
                resolvePacketName(shape.messageType()),
                ZLinkHandlerScanValidation.requireTopic(candidate, spotSubscription.topic()),
                "",
                null,
                groups));
        }
    }

    private static void addSpotActorHandlers(
        List<ZLinkScannedHandler> handlers,
        Class<?> candidate,
        Method method,
        Set<String> groups) {
        ZLinkSpotActorSend actorSend = method.getAnnotation(ZLinkSpotActorSend.class);
        if (actorSend != null) {
            requireJavaCompletionStageReturn(candidate, method);
            ActorMessageShape shape = requireActorPacketHandlerShape(
                candidate,
                method,
                ZLinkMessageContext.class);
            handlers.add(new ZLinkScannedHandler(
                ZLinkScannedHandlerSurface.SPOT,
                ZLinkScannedHandlerKind.ACTOR_SEND,
                candidate,
                method,
                shape.spotType(),
                shape.messageType(),
                Void.class,
                resolvePacketName(shape.messageType(), actorSend.packetName()),
                "",
                "",
                null,
                groups));
        }

        ZLinkSpotActorRequest actorRequest = method.getAnnotation(ZLinkSpotActorRequest.class);
        if (actorRequest != null) {
            requireJavaCompletionStageReturn(candidate, method);
            ActorMessageShape shape = requireActorPacketHandlerShape(
                candidate,
                method,
                ZLinkMessageContext.class);
            Class<?> replyType = resolveReplyType(candidate, method);
            handlers.add(new ZLinkScannedHandler(
                ZLinkScannedHandlerSurface.SPOT,
                ZLinkScannedHandlerKind.ACTOR_REQUEST,
                candidate,
                method,
                shape.spotType(),
                shape.messageType(),
                replyType,
                resolvePacketName(shape.messageType(), actorRequest.packetName()),
                "",
                "",
                null,
                groups));
        }
    }

    private static SpotMethodShape requireSpotMethodShape(
        Class<?> handlerType,
        Method method,
        Class<?> contextType,
        String failurePrefix) {
        Class<?>[] parameters = ZLinkHandlerMethodInvoker.logicalParameterTypes(method);
        if (parameters.length == 2) {
            return new SpotMethodShape(parameters[0], parameters[1]);
        }
        if (parameters.length == 3
            && parameters[2].isAssignableFrom(contextType)) {
            return new SpotMethodShape(parameters[0], parameters[1]);
        }
        throw new ZLinkConfigurationException(
            failurePrefix + handlerType.getName() + "." + method.getName());
    }

    private record SpotMethodShape(Class<?> spotType, Class<?> messageType) {
    }

    private static void requireJavaCompletionStageReturn(Class<?> handlerType, Method method) {
        if (ZLinkHandlerMethodInvoker.isKotlinSuspendMethod(method)) {
            return;
        }
        Type returnType = method.getGenericReturnType();
        if (returnType instanceof ParameterizedType parameterized
            && parameterized.getRawType() == java.util.concurrent.CompletionStage.class) {
            return;
        }
        throw new ZLinkConfigurationException(
            "Java handler method must return CompletionStage: "
                + handlerType.getName() + "." + method.getName());
    }

    private static void rejectConflictingSpotActorAnnotations(Class<?> handlerType, Method method) {
        if (method.getAnnotation(ZLinkSpotActorSend.class) != null
            && method.getAnnotation(ZLinkSpotActorRequest.class) != null) {
            throw new ZLinkConfigurationException(
                "SPOT actor handler method cannot declare both send and request annotations: "
                    + handlerType.getName() + "." + method.getName());
        }
    }

    private static ActorMessageShape requireActorPacketHandlerShape(
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

    private record ActorMessageShape(Class<?> spotType, Class<?> actorType, Class<?> messageType) {
    }

    private static Class<?> requireChannelHandlerShape(
        Class<?> handlerType,
        Method method,
        Class<?> contextType) {
        Class<?>[] parameters = ZLinkHandlerMethodInvoker.logicalParameterTypes(method);
        if (parameters.length == 0) {
            throw new ZLinkConfigurationException(
                "handler method must have a message parameter: "
                    + handlerType.getName() + "." + method.getName());
        }
        for (int index = 1; index < parameters.length; index++) {
            if (parameters[index].isAssignableFrom(contextType)) {
                continue;
            }
            throw new ZLinkConfigurationException(
                "handler method has unsupported parameter: "
                    + handlerType.getName() + "." + method.getName()
                    + " parameter " + index + " type " + parameters[index].getName());
        }
        return parameters[0];
    }

    private static Class<?> resolveReplyType(Class<?> handlerType, Method method) {
        if (ZLinkHandlerMethodInvoker.isKotlinSuspendMethod(method)) {
            return ZLinkHandlerMethodInvoker.kotlinSuspendReplyType(handlerType, method);
        }
        if (method.getReturnType() == Void.TYPE || method.getReturnType() == Void.class) {
            throw new ZLinkConfigurationException(
                "request handler method must return a reply: "
                    + handlerType.getName() + "." + method.getName());
        }
        Type returnType = method.getGenericReturnType();
        if (returnType instanceof ParameterizedType parameterized
            && parameterized.getRawType() == java.util.concurrent.CompletionStage.class) {
            Type replyType = parameterized.getActualTypeArguments()[0];
            if (replyType instanceof Class<?> replyClass && replyClass != Void.class) {
                return replyClass;
            }
            throw new ZLinkConfigurationException(
                "request handler CompletionStage must declare a concrete reply type: "
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

}
