package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.internal.metrics.ZLinkMeshMessageMetrics;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;

final class ZLinkSpotHandlerInvoker {
    private final ZLinkMessageSerializer serializer;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;

    ZLinkSpotHandlerInvoker(
            ZLinkMessageSerializer serializer,
            List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers) {
        this.serializer = serializer;
        this.suspendHandlerInvokers = suspendHandlerInvokers;
    }

    List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers() {
        return suspendHandlerInvokers;
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
        Object message;
        try {
            message = deserialize(payload, registration.messageType(), contentType);
        } catch (RuntimeException decodeFailure) {
            if (ZLinkRuntimeMetrics.enabled()) {
                ZLinkMeshMessageMetrics.forMesh(actor.context().meshName())
                        .dropped("actor", "decode_error");
            }
            throw decodeFailure;
        }
        ZLinkMessageContext context =
                new ZLinkSpotActorSendHandlerContext(
                        registration.packetName(), contentType, metadata);
        try {
            return registration
                    .invocation()
                    .invoke(
                            handlers.apply(registration.handlerType()),
                            spotSurface,
                            actor,
                            context,
                            message)
                    .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return failed(failureMessage + ": " + registration.handlerType().getName(), ex);
        }
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
        Object message = deserialize(payload, registration.messageType(), contentType);
        ZLinkMessageContext context =
                new ZLinkSpotActorRequestHandlerContext(
                        registration.packetName(), contentType, metadata);
        CompletionStage<Object> reply;
        try {
            reply =
                    registration
                            .invocation()
                            .invoke(
                                    handlers.apply(registration.handlerType()),
                                    spotSurface,
                                    actor,
                                    context,
                                    message);
        } catch (RuntimeException ex) {
            reply = failed(failureMessage + ": " + registration.handlerType().getName(), ex);
        }
        return reply.thenApply(
                value ->
                        Optional.of(
                                ZLinkMessagePayloads.message(
                                        ZLinkCodecRegistration.serializeForDeclaredType(
                                                serializer, value, registration.replyType()))));
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
        Object message = deserialize(payload, registration.messageType(), contentType);
        ZLinkMessageContext context =
                new ZLinkSpotSendHandlerContext(registration.packetName(), contentType, metadata);
        try {
            Object handler = handlers.apply(registration.handlerType());
            CompletionStage<?> stage =
                    registration.invocation().invoke(handler, spot, message, context);
            return stage.thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return failed(
                    "failed to invoke SPOT packet handler: " + registration.handlerType().getName(),
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
        Object message = deserialize(payload, registration.messageType(), contentType);
        ZLinkMessageContext context =
                new ZLinkSpotRequestHandlerContext(
                        registration.packetName(), contentType, metadata);
        try {
            Object handler = handlers.apply(registration.handlerType());
            CompletionStage<?> stage =
                    registration.invocation().invoke(handler, spot, message, context);
            return stage.thenApply(
                    reply ->
                            ZLinkMessagePayloads.message(
                                    ZLinkCodecRegistration.serializeForDeclaredType(
                                            serializer, reply, registration.replyType())));
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
        Object message = deserializeSubscription(registration, payloadOwner(payload, contentType));
        return invokeSubscriptionDecoded(
                registration,
                spot,
                channelName,
                topic,
                source,
                message,
                contentType,
                metadata,
                handlers);
    }

    ZLinkInboundPayloadOwner payloadOwner(Message payload, String contentType) {
        ZLinkMessageSerializer selected =
                contentType == null
                        ? serializer
                        : ZLinkCodecRegistration.serializerForReceivedContentType(
                                serializer, contentType);
        return new ZLinkInboundPayloadOwner(payload, selected);
    }

    Object deserializeSubscription(
            SpotSubscriptionHandlerRegistration registration, ZLinkInboundPayloadOwner payload) {
        return payload.deserialize(registration.messageType());
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<Void> invokeSubscriptionDecoded(
            SpotSubscriptionHandlerRegistration registration,
            Object spot,
            String channelName,
            String topic,
            Optional<String> source,
            Object message,
            String contentType,
            Map<String, String> metadata,
            Function<Class<?>, Object> handlers) {
        ZLinkPublishMessageContext context =
                new ZLinkSpotPublishHandlerContext(
                        channelName,
                        registration.packetName(),
                        topic,
                        contentType,
                        source,
                        metadata);
        try {
            Object handler = handlers.apply(registration.handlerType());
            CompletionStage<?> stage =
                    registration.invocation().invoke(handler, spot, message, context);
            return stage.thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return failed(
                    "failed to invoke SPOT subscription handler: "
                            + registration.handlerType().getName(),
                    ex);
        }
    }

    private Object deserialize(Message payload, Class<?> messageType, String contentType) {
        ZLinkMessageSerializer selected =
                contentType == null
                        ? serializer
                        : ZLinkCodecRegistration.serializerForReceivedContentType(
                                serializer, contentType);
        return ZLinkMessagePayloads.deserialize(selected, payload, messageType);
    }

    private static <T> CompletionStage<T> failed(String message, RuntimeException error) {
        return CompletableFuture.failedFuture(new ZLinkConfigurationException(message, error));
    }
}
