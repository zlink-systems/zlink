package systems.zlink.framework.runtime.spots;

import java.lang.reflect.Method;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;

final class ZLinkSpotHandlerInvoker {
    private final ZLinkMessageSerializer serializer;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;

    ZLinkSpotHandlerInvoker(
        ZLinkMessageSerializer serializer,
        List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers) {
        this.serializer = serializer;
        this.suspendHandlerInvokers = suspendHandlerInvokers;
    }

    CompletionStage<Void> invokeActorSend(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers,
        String failureMessage) {
        Object message = deserialize(payload, registration.messageType());
        ZLinkMessageContext context =
            new ZLinkSpotActorSendHandlerContext(
                registration.packetName(), contentType, metadata);
        if (registration.handlerMethod() == null) {
            return invokeActorSendInterface(
                registration,
                spotSurface,
                actor,
                context,
                message,
                handlers,
                failureMessage);
        }
        return invokeVoidMethod(
            registration.handlerType(),
            registration.handlerMethod(),
            actorPacketArguments(
                registration.handlerMethod(),
                spotSurface,
                actor,
                context,
                message),
            handlers,
            failureMessage);
    }

    CompletionStage<Void> invokeActorSend(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers,
        String failureMessage) {
        return invokeActorSend(
            registration,
            spotSurface,
            actor,
            payload,
            null,
            metadata,
            handlers,
            failureMessage);
    }

    CompletionStage<Optional<Message>> invokeActorRequest(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers,
        String failureMessage) {
        Object message = deserialize(payload, registration.messageType());
        ZLinkMessageContext context =
            new ZLinkSpotActorRequestHandlerContext(
                registration.packetName(), contentType, metadata);
        CompletionStage<Object> reply = registration.handlerMethod() == null
            ? invokeActorRequestInterface(
                registration,
                spotSurface,
                actor,
                context,
                message,
                handlers,
                failureMessage)
            : invokeReplyMethod(
                registration.handlerType(),
                registration.handlerMethod(),
                actorPacketArguments(
                    registration.handlerMethod(),
                    spotSurface,
                    actor,
                    context,
                    message),
                handlers,
                failureMessage);
        return reply.thenApply(value ->
            Optional.of(ZLinkMessagePayloads.message(serializer.serialize(value))));
    }

    CompletionStage<Optional<Message>> invokeActorRequest(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        Message payload,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers,
        String failureMessage) {
        return invokeActorRequest(
            registration,
            spotSurface,
            actor,
            payload,
            null,
            metadata,
            handlers,
            failureMessage);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Void> invokePacket(
        SpotPacketHandlerRegistration registration,
        Object spot,
        Message payload,
        Function<Class<?>, Object> handlers) {
        return invokePacket(registration, spot, payload, null, Map.of(), handlers);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Void> invokePacket(
        SpotPacketHandlerRegistration registration,
        Object spot,
        Message payload,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers) {
        return invokePacket(registration, spot, payload, null, metadata, handlers);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Void> invokePacket(
        SpotPacketHandlerRegistration registration,
        Object spot,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers) {
        Object message = deserialize(payload, registration.messageType());
        ZLinkMessageContext context = new ZLinkSpotSendHandlerContext(
            registration.packetName(), contentType, metadata);
        try {
            Object handler = handlers.apply(registration.handlerType());
            CompletionStage<?> stage = registration.handlerMethod() != null
                ? ZLinkHandlerMethodInvoker.invoke(
                    handler,
                    registration.handlerMethod(),
                    spotMessageArguments(
                        registration.handlerMethod(), spot, message, context),
                    suspendHandlerInvokers)
                : ZLinkHandlerMethodInvoker.invokeHandler(
                    handler,
                    "handle",
                    new Object[] {spot, message, context},
                    suspendHandlerInvokers);
            return stage.thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return failed(
                "failed to invoke SPOT packet handler: "
                    + registration.handlerType().getName(),
                ex);
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Message> invokeRequest(
        SpotPacketHandlerRegistration registration,
        Object spot,
        Message payload,
        Function<Class<?>, Object> handlers) {
        return invokeRequest(registration, spot, payload, null, Map.of(), handlers);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Message> invokeRequest(
        SpotPacketHandlerRegistration registration,
        Object spot,
        Message payload,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers) {
        return invokeRequest(registration, spot, payload, null, metadata, handlers);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Message> invokeRequest(
        SpotPacketHandlerRegistration registration,
        Object spot,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers) {
        Object message = deserialize(payload, registration.messageType());
        ZLinkMessageContext context = new ZLinkSpotRequestHandlerContext(
            registration.packetName(), contentType, metadata);
        try {
            Object handler = handlers.apply(registration.handlerType());
            CompletionStage<?> stage = registration.handlerMethod() != null
                ? ZLinkHandlerMethodInvoker.invoke(
                    handler,
                    registration.handlerMethod(),
                    spotMessageArguments(
                        registration.handlerMethod(), spot, message, context),
                    suspendHandlerInvokers)
                : ZLinkHandlerMethodInvoker.invokeHandler(
                    handler,
                    "handle",
                    new Object[] {spot, message, context},
                    suspendHandlerInvokers);
            return stage.thenApply(reply ->
                ZLinkMessagePayloads.message(serializer.serialize(reply)));
        } catch (RuntimeException ex) {
            return failed(
                "failed to invoke SPOT request handler: "
                    + registration.handlerType().getName(),
                ex);
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Void> invokeSubscription(
        SpotSubscriptionHandlerRegistration registration,
        Object spot,
        Message payload,
        Function<Class<?>, Object> handlers) {
        return invokeSubscription(
            registration,
            spot,
            null,
            null,
            Optional.empty(),
            payload,
            null,
            Map.of(),
            handlers);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Void> invokeSubscription(
        SpotSubscriptionHandlerRegistration registration,
        Object spot,
        String channelName,
        String topic,
        Optional<String> source,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        Function<Class<?>, Object> handlers) {
        Object message = deserialize(payload, registration.messageType());
        ZLinkPublishMessageContext context = new ZLinkSpotPublishHandlerContext(
            channelName,
            registration.packetName(),
            topic,
            contentType,
            source,
            metadata);
        try {
            Object handler = handlers.apply(registration.handlerType());
            CompletionStage<?> stage = registration.handlerMethod() != null
                ? ZLinkHandlerMethodInvoker.invoke(
                    handler,
                    registration.handlerMethod(),
                    spotMessageArguments(
                        registration.handlerMethod(), spot, message, context),
                    suspendHandlerInvokers)
                : ZLinkHandlerMethodInvoker.invokeHandler(
                    handler,
                    "handle",
                    new Object[] {spot, message, context},
                    suspendHandlerInvokers);
            return stage.thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return failed(
                "failed to invoke SPOT subscription handler: "
                    + registration.handlerType().getName(),
                ex);
        }
    }

    static Object[] actorPacketArguments(
        Method method,
        Object spot,
        ZLinkActor actor,
        ZLinkMessageContext context,
        Object message) {
        Class<?>[] parameterTypes = ZLinkHandlerMethodInvoker.logicalParameterTypes(method);
        if (parameterTypes.length == 2) {
            return new Object[] {actor, message};
        }
        return new Object[] {spot, actor, context, message};
    }

    private static Object[] spotMessageArguments(
        Method method,
        Object spot,
        Object message,
        ZLinkMessageContext context) {
        Class<?>[] parameterTypes =
            ZLinkHandlerMethodInvoker.logicalParameterTypes(method);
        if (parameterTypes.length == 2) {
            return new Object[] {spot, message};
        }
        Object[] arguments = new Object[parameterTypes.length];
        for (int index = 0; index < parameterTypes.length; index++) {
            if (parameterTypes[index].isInstance(context)) {
                arguments[index] = context;
            } else if (parameterTypes[index].isInstance(spot)) {
                arguments[index] = spot;
            } else {
                arguments[index] = message;
            }
        }
        return arguments;
    }

    private CompletionStage<Void> invokeVoidMethod(
        Class<?> handlerType,
        Method method,
        Object[] arguments,
        Function<Class<?>, Object> handlers,
        String failureMessage) {
        return invokeReplyMethod(handlerType, method, arguments, handlers, failureMessage)
            .thenApply(ignored -> null);
    }

    private CompletionStage<Object> invokeReplyMethod(
        Class<?> handlerType,
        Method method,
        Object[] arguments,
        Function<Class<?>, Object> handlers,
        String failureMessage) {
        try {
            Object handler = handlers.apply(handlerType);
            return ZLinkHandlerMethodInvoker.invoke(
                handler,
                method,
                arguments,
                suspendHandlerInvokers);
        } catch (RuntimeException ex) {
            return failed(
                failureMessage + ": " + handlerType.getName() + "." + method.getName(),
                ex);
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private CompletionStage<Void> invokeActorSendInterface(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        ZLinkMessageContext context,
        Object message,
        Function<Class<?>, Object> handlers,
        String failureMessage) {
        try {
            Object handler = handlers.apply(registration.handlerType());
            return ZLinkHandlerMethodInvoker
                .invokeHandler(
                    handler,
                    "handle",
                    new Object[] {spotSurface, actor, context, message},
                    suspendHandlerInvokers)
                .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return failed(
                failureMessage + ": " + registration.handlerType().getName(),
                ex);
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private CompletionStage<Object> invokeActorRequestInterface(
        SpotActorPacketHandlerRegistration registration,
        Object spotSurface,
        ZLinkActor actor,
        ZLinkMessageContext context,
        Object message,
        Function<Class<?>, Object> handlers,
        String failureMessage) {
        try {
            Object handler = handlers.apply(registration.handlerType());
            return ZLinkHandlerMethodInvoker.invokeHandler(
                handler,
                "handle",
                new Object[] {spotSurface, actor, context, message},
                suspendHandlerInvokers);
        } catch (RuntimeException ex) {
            return failed(
                failureMessage + ": " + registration.handlerType().getName(),
                ex);
        }
    }

    private Object deserialize(Message payload, Class<?> messageType) {
        return ZLinkMessagePayloads.deserialize(serializer, payload, messageType);
    }

    private static <T> CompletionStage<T> failed(String message, RuntimeException error) {
        return CompletableFuture.failedFuture(new ZLinkConfigurationException(message, error));
    }

}
