package systems.zlink.framework.runtime.channels;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkHandlerDispatchKind;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Function;
import java.util.function.Supplier;

final class ZLinkChannelHandlerInvoker {
    private final ZLinkMessageSerializer serializer;
    private final ZLinkCodecRegistration codecs;
    private final ZLinkHandlerActivator handlerFactory;
    private final List<Class<? extends ZLinkHandlerFilter>> filterTypes;
    private final String meshName;

    ZLinkChannelHandlerInvoker(
            ZLinkMessageSerializer serializer,
            ZLinkCodecRegistration codecs,
            ZLinkHandlerActivator handlerFactory,
            Executor handlerExecutor,
            List<Class<? extends ZLinkHandlerFilter>> filterTypes) {
        this(serializer, codecs, handlerFactory, handlerExecutor, filterTypes, null);
    }

    ZLinkChannelHandlerInvoker(
            ZLinkMessageSerializer serializer,
            ZLinkCodecRegistration codecs,
            ZLinkHandlerActivator handlerFactory,
            Executor handlerExecutor,
            List<Class<? extends ZLinkHandlerFilter>> filterTypes,
            String meshName) {
        this.serializer = serializer;
        this.codecs = codecs;
        this.handlerFactory = handlerFactory;
        Objects.requireNonNull(handlerExecutor, "handlerExecutor");
        this.filterTypes = filterTypes;
        this.meshName = meshName;
    }

    <T> CompletionStage<T> executeHandler(Supplier<CompletionStage<T>> operation) {
        try {
            return ZLinkFlowContext.propagate(
                    Objects.requireNonNull(operation.get(), "handler stage"));
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokeSendHandler(
            String channelName, ChannelSendHandlerRegistration registration, Message payload) {
        return invokeSendHandler(channelName, registration, payload, Map.of());
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokeSendHandler(
            String channelName,
            ChannelSendHandlerRegistration registration,
            Message payload,
            Map<String, String> metadata) {
        return invokeSendHandler(channelName, registration, payload, metadata, null);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokeSendHandler(
            String channelName,
            ChannelSendHandlerRegistration registration,
            Message payload,
            Map<String, String> metadata,
            String wireContentType) {
        Object message;
        try {
            message = deserializePayload(payload, registration.messageType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(
                    payloadDecodeFailure(channelName, registration.packetName(), ex));
        }
        try {
            ZLinkMessageContext context =
                    new DefaultSendContext(
                            meshName,
                            channelName,
                            registration.packetName(),
                            contentTypeFor(registration.messageType(), wireContentType),
                            metadata);
            return withDispatchHandlers(
                    handlers ->
                            invokeWithFilters(
                                            ZLinkHandlerDispatchKind.CHANNEL_SEND,
                                            context,
                                            handlers,
                                            () ->
                                                    invokeSendHandlerCore(
                                                            registration,
                                                            message,
                                                            context,
                                                            handlers))
                                    .thenApply(ignored -> null));
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private CompletionStage<Void> invokeSendHandlerCore(
            ChannelSendHandlerRegistration registration,
            Object message,
            ZLinkMessageContext context,
            ZLinkHandlerInstanceOwner handlers) {
        try {
            Object handler = handlers.instance(registration.handlerType());
            return registration
                    .invocation()
                    .invoke(handler, message, context)
                    .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Message> invokeRequestHandler(
            String channelName, ChannelRequestHandlerRegistration registration, Message payload) {
        return invokeRequestHandler(channelName, registration, payload, Map.of());
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Message> invokeRequestHandler(
            String channelName,
            ChannelRequestHandlerRegistration registration,
            Message payload,
            Map<String, String> metadata) {
        return invokeRequestHandler(channelName, registration, payload, metadata, null);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Message> invokeRequestHandler(
            String channelName,
            ChannelRequestHandlerRegistration registration,
            Message payload,
            Map<String, String> metadata,
            String wireContentType) {
        Object request;
        try {
            request = deserializePayload(payload, registration.requestType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(
                    payloadDecodeFailure(channelName, registration.packetName(), ex));
        }
        try {
            ZLinkMessageContext context =
                    new DefaultRequestContext(
                            meshName,
                            channelName,
                            registration.packetName(),
                            contentTypeFor(registration.requestType(), wireContentType),
                            metadata);
            return withDispatchHandlers(
                            handlers ->
                                    invokeRequestWithFilters(
                                            ZLinkHandlerDispatchKind.CHANNEL_REQUEST,
                                            context,
                                            handlers,
                                            () ->
                                                    invokeRequestHandlerCore(
                                                            registration,
                                                            request,
                                                            context,
                                                            handlers)))
                    .thenApply(
                            reply ->
                                    ZLinkMessagePayloads.message(
                                            serializer, reply, registration.replyType()));
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private CompletionStage<Object> invokeRequestHandlerCore(
            ChannelRequestHandlerRegistration registration,
            Object request,
            ZLinkMessageContext context,
            ZLinkHandlerInstanceOwner handlers) {
        try {
            Object handler = handlers.instance(registration.handlerType());
            return registration.invocation().invoke(handler, request, context);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokePublishHandler(
            String channelName,
            ChannelPublishHandlerRegistration registration,
            String topic,
            Message payload) {
        return invokePublishHandler(channelName, registration, topic, payload, null);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokePublishHandler(
            String channelName,
            ChannelPublishHandlerRegistration registration,
            String topic,
            Message payload,
            String wireContentType) {
        Object message;
        try {
            message = deserializePayload(payload, registration.messageType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(
                    payloadDecodeFailure(channelName, registration.packetName(), ex));
        }
        try {
            ZLinkPublishMessageContext context =
                    new DefaultPublishContext(
                            channelName,
                            registration.packetName(),
                            topic,
                            contentTypeFor(registration.messageType(), wireContentType));
            return withDispatchHandlers(
                    handlers ->
                            invokeWithFilters(
                                            ZLinkHandlerDispatchKind.CLASSIC_FANOUT,
                                            context,
                                            handlers,
                                            () ->
                                                    invokePublishHandlerCore(
                                                            registration,
                                                            message,
                                                            context,
                                                            handlers))
                                    .thenApply(ignored -> null));
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private CompletionStage<Void> invokePublishHandlerCore(
            ChannelPublishHandlerRegistration registration,
            Object message,
            ZLinkPublishMessageContext context,
            ZLinkHandlerInstanceOwner handlers) {
        try {
            Object handler = handlers.instance(registration.handlerType());
            return registration
                    .invocation()
                    .invoke(handler, message, context)
                    .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    CompletionStage<Void> invokeRouteSendHandler(
            String channelName,
            ChannelRouteSendHandlerRegistration registration,
            RoutingId sourceRoutingId,
            Message payload) {
        return invokeRouteSendHandler(
                channelName, registration, sourceRoutingId, payload, Map.of());
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokeRouteSendHandler(
            String channelName,
            ChannelRouteSendHandlerRegistration registration,
            RoutingId sourceRoutingId,
            Message payload,
            Map<String, String> metadata) {
        return invokeRouteSendHandler(
                channelName, registration, sourceRoutingId, payload, metadata, null);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokeRouteSendHandler(
            String channelName,
            ChannelRouteSendHandlerRegistration registration,
            RoutingId sourceRoutingId,
            Message payload,
            Map<String, String> metadata,
            String wireContentType) {
        Object message;
        try {
            message = deserializePayload(payload, registration.messageType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(
                    payloadDecodeFailure(channelName, registration.packetName(), ex));
        }
        try {
            ZLinkRouteMessageContext context =
                    new DefaultRouteSendContext(
                            routeMeshName(channelName),
                            meshName == null || meshName.isBlank() ? null : channelName,
                            registration.packetName(),
                            sourceRoutingId,
                            contentTypeFor(registration.messageType(), wireContentType),
                            metadata);
            return withDispatchHandlers(
                    handlers ->
                            invokeWithFilters(
                                            ZLinkHandlerDispatchKind.NODE_DIRECT_SEND,
                                            context,
                                            handlers,
                                            () ->
                                                    invokeRouteSendHandlerCore(
                                                            registration,
                                                            message,
                                                            context,
                                                            handlers))
                                    .thenApply(ignored -> null));
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Message> invokeRouteRequestHandler(
            String channelName,
            ChannelRouteRequestHandlerRegistration registration,
            RoutingId sourceRoutingId,
            Message payload) {
        return invokeRouteRequestHandler(
                channelName, registration, sourceRoutingId, payload, Map.of());
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Message> invokeRouteRequestHandler(
            String channelName,
            ChannelRouteRequestHandlerRegistration registration,
            RoutingId sourceRoutingId,
            Message payload,
            Map<String, String> metadata) {
        return invokeRouteRequestHandler(
                channelName, registration, sourceRoutingId, payload, metadata, null);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Message> invokeRouteRequestHandler(
            String channelName,
            ChannelRouteRequestHandlerRegistration registration,
            RoutingId sourceRoutingId,
            Message payload,
            Map<String, String> metadata,
            String wireContentType) {
        Object request;
        try {
            request = deserializePayload(payload, registration.requestType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(
                    payloadDecodeFailure(channelName, registration.packetName(), ex));
        }
        try {
            ZLinkRouteMessageContext context =
                    new DefaultRouteRequestContext(
                            routeMeshName(channelName),
                            null,
                            registration.packetName(),
                            sourceRoutingId,
                            contentTypeFor(registration.requestType(), wireContentType),
                            metadata);
            return withDispatchHandlers(
                    handlers ->
                            invokeRequestWithFilters(
                                            ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST,
                                            context,
                                            handlers,
                                            () ->
                                                    invokeRouteRequestHandlerCore(
                                                            registration,
                                                            request,
                                                            context,
                                                            handlers))
                                    .thenApply(
                                            reply ->
                                                    ZLinkMessagePayloads.message(
                                                            serializer,
                                                            reply,
                                                            registration.replyType())));
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private static PayloadDecodeDispatchException payloadDecodeFailure(
            String channelName, String packetName, RuntimeException cause) {
        return new PayloadDecodeDispatchException(
                "PayloadDecodeFailed: failed to decode payload for '"
                        + channelName
                        + ":"
                        + packetName
                        + "'.",
                cause);
    }

    private String contentTypeFor(Class<?> payloadType) {
        return codecs.contentTypeFor(payloadType);
    }

    private String contentTypeFor(Class<?> payloadType, String wireContentType) {
        return wireContentType == null ? contentTypeFor(payloadType) : wireContentType;
    }

    private <T> T deserializePayload(
            Message payload, Class<T> payloadType, String wireContentType) {
        ZLinkMessageSerializer selected =
                wireContentType == null
                        ? serializer
                        : ZLinkCodecRegistration.serializerForReceivedContentType(
                                serializer, wireContentType);
        return ZLinkMessagePayloads.deserialize(selected, payload, payloadType);
    }

    private String routeMeshName(String legacyChannelName) {
        return meshName == null || meshName.isBlank() ? legacyChannelName : meshName;
    }

    private CompletionStage<Void> invokeRouteSendHandlerCore(
            ChannelRouteSendHandlerRegistration registration,
            Object message,
            ZLinkRouteMessageContext context,
            ZLinkHandlerInstanceOwner handlers) {
        try {
            Object handler = handlers.instance(registration.handlerType());
            return registration
                    .invocation()
                    .invoke(handler, message, context)
                    .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private CompletionStage<Object> invokeRouteRequestHandlerCore(
            ChannelRouteRequestHandlerRegistration registration,
            Object request,
            ZLinkRouteMessageContext context,
            ZLinkHandlerInstanceOwner handlers) {
        try {
            Object handler = handlers.instance(registration.handlerType());
            return registration.invocation().invoke(handler, request, context);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private <T> CompletionStage<ZLinkFilterPipeline.Result<T>> invokeWithFilters(
            ZLinkHandlerDispatchKind dispatchKind,
            ZLinkMessageContext context,
            ZLinkHandlerInstanceOwner handlers,
            Supplier<CompletionStage<T>> terminal) {
        if (filterTypes.isEmpty()) {
            return terminal.get().thenApply(value -> new ZLinkFilterPipeline.Result<>(true, value));
        }
        return ZLinkFilterPipeline.invoke(
                filterTypes,
                handlers,
                new DefaultHandlerFilterContext(context, dispatchKind),
                terminal);
    }

    private <T> CompletionStage<T> invokeRequestWithFilters(
            ZLinkHandlerDispatchKind dispatchKind,
            ZLinkMessageContext context,
            ZLinkHandlerInstanceOwner handlers,
            Supplier<CompletionStage<T>> terminal) {
        return invokeWithFilters(dispatchKind, context, handlers, terminal)
                .thenCompose(
                        result -> {
                            if (!result.handlerInvoked()) {
                                return CompletableFuture.failedFuture(
                                        new ZLinkFrameworkException(
                                                ZLinkFrameworkErrorKind.REJECTED,
                                                "A handler filter rejected '"
                                                        + context.packetName()
                                                        + "'."));
                            }
                            return CompletableFuture.completedFuture(result.value());
                        });
    }

    private <T> CompletionStage<T> withDispatchHandlers(
            Function<ZLinkHandlerInstanceOwner, CompletionStage<T>> operation) {
        ZLinkHandlerInstanceOwner handlers = new ZLinkHandlerInstanceOwner(handlerFactory);
        try {
            return operation.apply(handlers).whenComplete((ignored, error) -> handlers.close());
        } catch (RuntimeException error) {
            handlers.close();
            throw error;
        }
    }
}
