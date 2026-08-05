package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.backend.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkHandlerDispatchKind;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkClientServerChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.configuration.ZLinkDispatchFailure;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

final class ZLinkChannelHandlerInvoker {
    private final ZLinkMessageSerializer serializer;
    private final ZLinkCodecRegistration codecs;
    private final ZLinkHandlerActivator handlerFactory;
    private final Executor handlerExecutor;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;
    private final List<Class<? extends ZLinkHandlerFilter>> filterTypes;
    private final String meshName;

    ZLinkChannelHandlerInvoker(
        ZLinkMessageSerializer serializer,
        ZLinkCodecRegistration codecs,
        ZLinkHandlerActivator handlerFactory,
        Executor handlerExecutor,
        List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers,
        List<Class<? extends ZLinkHandlerFilter>> filterTypes) {
        this(
            serializer,
            codecs,
            handlerFactory,
            handlerExecutor,
            suspendHandlerInvokers,
            filterTypes,
            null);
    }

    ZLinkChannelHandlerInvoker(
        ZLinkMessageSerializer serializer,
        ZLinkCodecRegistration codecs,
        ZLinkHandlerActivator handlerFactory,
        Executor handlerExecutor,
        List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers,
        List<Class<? extends ZLinkHandlerFilter>> filterTypes,
        String meshName) {
        this.serializer = serializer;
        this.codecs = codecs;
        this.handlerFactory = handlerFactory;
        this.handlerExecutor = handlerExecutor;
        this.suspendHandlerInvokers = suspendHandlerInvokers;
        this.filterTypes = filterTypes;
        this.meshName = meshName;
    }

    <T> CompletionStage<T> executeHandler(
        java.util.function.Supplier<CompletionStage<T>> operation) {
        CompletableFuture<T> result = new CompletableFuture<>();
        var flow = systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.current();
        try {
            handlerExecutor.execute(() -> {
                try {
                    operation.get().whenComplete((value, error) -> {
                        systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.run(flow, () -> {
                            if (error != null) {
                                result.completeExceptionally(error);
                            } else {
                                result.complete(value);
                            }
                        });
                    });
                } catch (RuntimeException ex) {
                    result.completeExceptionally(ex);
                }
            });
        } catch (RuntimeException ex) {
            result.completeExceptionally(ex);
        }
        return result;
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokeSendHandler(
        String channelName,
        ChannelSendHandlerRegistration registration,
        Message payload) {
        return invokeSendHandler(channelName, registration, payload, Map.of());
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Void> invokeSendHandler(
        String channelName,
        ChannelSendHandlerRegistration registration,
        Message payload,
        Map<String, String> metadata) {
        return invokeSendHandler(
            channelName, registration, payload, metadata, null);
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
            message = deserializePayload(
                payload, registration.messageType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(payloadDecodeFailure(
                channelName,
                registration.packetName(),
                ex));
        }
        try {
            ZLinkMessageContext context = new DefaultSendContext(
                meshName,
                channelName,
                registration.packetName(),
                contentTypeFor(registration.messageType(), wireContentType),
                metadata);
            return withDispatchHandlers(handlers ->
                invokeWithFilters(
                    ZLinkHandlerDispatchKind.CHANNEL_SEND,
                    context,
                    handlers,
                    () -> invokeSendHandlerCore(
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
            if (registration.handlerMethod() != null) {
                return invokeVoidMethodHandler(
                    registration.handlerType(),
                    registration.handlerMethod(),
                    message,
                    context,
                    handlers);
            }
            Object handler = handlers.instance(registration.handlerType());
            return ZLinkHandlerMethodInvoker
                .invokeHandler(handler, "handle", new Object[] {message, context}, suspendHandlerInvokers)
                .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Message> invokeRequestHandler(
        String channelName,
        ChannelRequestHandlerRegistration registration,
        Message payload) {
        return invokeRequestHandler(channelName, registration, payload, Map.of());
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    CompletionStage<Message> invokeRequestHandler(
        String channelName,
        ChannelRequestHandlerRegistration registration,
        Message payload,
        Map<String, String> metadata) {
        return invokeRequestHandler(
            channelName, registration, payload, metadata, null);
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
            request = deserializePayload(
                payload, registration.requestType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(payloadDecodeFailure(
                channelName,
                registration.packetName(),
                ex));
        }
        try {
            ZLinkMessageContext context = new DefaultRequestContext(
                meshName,
                channelName,
                registration.packetName(),
                contentTypeFor(registration.requestType(), wireContentType),
                metadata);
            return withDispatchHandlers(handlers ->
                invokeRequestWithFilters(
                    ZLinkHandlerDispatchKind.CHANNEL_REQUEST,
                    context,
                    handlers,
                    () -> invokeRequestHandlerCore(
                        registration,
                        request,
                        context,
                        handlers)))
                .thenApply(reply -> ZLinkMessagePayloads.message(serializer.serialize(reply)));
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
            if (registration.handlerMethod() != null) {
                return invokeReplyMethodHandler(
                    registration.handlerType(),
                    registration.handlerMethod(),
                    request,
                    context,
                    handlers);
            }
            Object handler = handlers.instance(registration.handlerType());
            return ZLinkHandlerMethodInvoker
                .invokeHandler(handler, "handle", new Object[] {request, context}, suspendHandlerInvokers);
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
        return invokePublishHandler(
            channelName, registration, topic, payload, null);
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
            message = deserializePayload(
                payload, registration.messageType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(payloadDecodeFailure(
                channelName,
                registration.packetName(),
                ex));
        }
        try {
            ZLinkPublishMessageContext context = new DefaultPublishContext(
                channelName,
                registration.packetName(),
                topic,
                contentTypeFor(registration.messageType(), wireContentType));
            return withDispatchHandlers(handlers ->
                invokeWithFilters(
                    ZLinkHandlerDispatchKind.CLASSIC_FANOUT,
                    context,
                    handlers,
                    () -> invokePublishHandlerCore(
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
            if (registration.handlerMethod() != null) {
                return invokeVoidMethodHandler(
                    registration.handlerType(),
                    registration.handlerMethod(),
                    message,
                    context,
                    handlers);
            }
            Object handler = handlers.instance(registration.handlerType());
            return ZLinkHandlerMethodInvoker
                .invokeHandler(handler, "handle", new Object[] {message, context}, suspendHandlerInvokers)
                .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private CompletionStage<Void> invokeVoidMethodHandler(
        Class<?> handlerType,
        Method method,
        Object message,
        ZLinkMessageContext context,
        ZLinkHandlerInstanceOwner handlers) {
        try {
            Object handler = handlers.instance(handlerType);
            return ZLinkHandlerMethodInvoker
                .invoke(handler, method, methodArguments(method, message, context), suspendHandlerInvokers)
                .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "failed to invoke handler method: " + handlerType.getName() + "." + method.getName(),
                ex));
        }
    }

    private CompletionStage<Object> invokeReplyMethodHandler(
        Class<?> handlerType,
        Method method,
        Object message,
        ZLinkMessageContext context,
        ZLinkHandlerInstanceOwner handlers) {
        try {
            Object handler = handlers.instance(handlerType);
            return ZLinkHandlerMethodInvoker.invoke(
                handler,
                method,
                methodArguments(method, message, context),
                suspendHandlerInvokers);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "failed to invoke handler method: " + handlerType.getName() + "." + method.getName(),
                ex));
        }
    }

    static Object[] methodArguments(
        Method method,
        Object message,
        ZLinkMessageContext context) {
        Class<?>[] parameterTypes = ZLinkHandlerMethodInvoker.logicalParameterTypes(method);
        Object[] arguments = new Object[parameterTypes.length];
        arguments[0] = message;
        for (int index = 1; index < parameterTypes.length; index++) {
            if (parameterTypes[index].isInstance(context)) {
                arguments[index] = context;
            } else {
                arguments[index] = null;
            }
        }
        return arguments;
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
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
            message = deserializePayload(
                payload, registration.messageType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(payloadDecodeFailure(
                channelName,
                registration.packetName(),
                ex));
        }
        try {
            ZLinkRouteMessageContext context =
                new DefaultRouteSendContext(
                    routeMeshName(channelName),
                    null,
                    registration.packetName(),
                    sourceRoutingId,
                    contentTypeFor(registration.messageType(), wireContentType),
                    metadata);
            return withDispatchHandlers(handlers ->
                invokeWithFilters(
                    ZLinkHandlerDispatchKind.NODE_DIRECT_SEND,
                    context,
                    handlers,
                    () -> invokeRouteSendHandlerCore(
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
            request = deserializePayload(
                payload, registration.requestType(), wireContentType);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(payloadDecodeFailure(
                channelName,
                registration.packetName(),
                ex));
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
            return withDispatchHandlers(handlers ->
                invokeRequestWithFilters(
                    ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST,
                    context,
                    handlers,
                    () -> invokeRouteRequestHandlerCore(
                        registration,
                        request,
                        context,
                        handlers))
                    .thenApply(reply ->
                        ZLinkMessagePayloads.message(serializer.serialize(reply))));
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private static PayloadDecodeDispatchException payloadDecodeFailure(
        String channelName,
        String packetName,
        RuntimeException cause) {
        return new PayloadDecodeDispatchException(
            "PayloadDecodeFailed: failed to decode payload for '" + channelName + ":" + packetName + "'.",
            cause);
    }

    private String contentTypeFor(Class<?> payloadType) {
        return codecs.contentTypeFor(payloadType);
    }

    private String contentTypeFor(Class<?> payloadType, String wireContentType) {
        return wireContentType == null
            ? contentTypeFor(payloadType)
            : wireContentType;
    }

    private <T> T deserializePayload(
        Message payload,
        Class<T> payloadType,
        String wireContentType) {
        ZLinkMessageSerializer selected = wireContentType == null
            ? serializer
            : codecs.serializerForReceivedContentType(
                wireContentType, serializer);
        return ZLinkMessagePayloads.deserialize(selected, payload, payloadType);
    }

    private String routeMeshName(String legacyChannelName) {
        return meshName == null || meshName.isBlank()
            ? legacyChannelName
            : meshName;
    }

    private CompletionStage<Void> invokeRouteSendHandlerCore(
        ChannelRouteSendHandlerRegistration registration,
        Object message,
        ZLinkRouteMessageContext context,
        ZLinkHandlerInstanceOwner handlers) {
        Object handler = handlers.instance(registration.handlerType());
        if (registration.handlerMethod() != null) {
            return ZLinkHandlerMethodInvoker
                .invoke(
                    handler,
                    registration.handlerMethod(),
                    methodArguments(registration.handlerMethod(), message, context),
                    suspendHandlerInvokers)
                .thenApply(ignored -> null);
        }
        return ZLinkHandlerMethodInvoker
            .invokeHandler(
                handler,
                "handle",
                new Object[] {message, context},
                suspendHandlerInvokers)
            .thenApply(ignored -> null);
    }

    private CompletionStage<Object> invokeRouteRequestHandlerCore(
        ChannelRouteRequestHandlerRegistration registration,
        Object request,
        ZLinkRouteMessageContext context,
        ZLinkHandlerInstanceOwner handlers) {
        Object handler = handlers.instance(registration.handlerType());
        if (registration.handlerMethod() != null) {
            return ZLinkHandlerMethodInvoker.invoke(
                handler,
                registration.handlerMethod(),
                methodArguments(registration.handlerMethod(), request, context),
                suspendHandlerInvokers);
        }
        return ZLinkHandlerMethodInvoker.invokeHandler(
            handler,
            "handle",
            new Object[] {request, context},
            suspendHandlerInvokers);
    }

    private <T> CompletionStage<ZLinkFilterPipeline.Result<T>> invokeWithFilters(
        ZLinkHandlerDispatchKind dispatchKind,
        ZLinkMessageContext context,
        ZLinkHandlerInstanceOwner handlers,
        java.util.function.Supplier<CompletionStage<T>> terminal) {
        if (filterTypes.isEmpty()) {
            return terminal.get().thenApply(
                value -> new ZLinkFilterPipeline.Result<>(true, value));
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
        java.util.function.Supplier<CompletionStage<T>> terminal) {
        return invokeWithFilters(
            dispatchKind,
            context,
            handlers,
            terminal).thenCompose(result -> {
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
        java.util.function.Function<
            ZLinkHandlerInstanceOwner, CompletionStage<T>> operation) {
        ZLinkHandlerInstanceOwner handlers =
            new ZLinkHandlerInstanceOwner(handlerFactory);
        try {
            return operation.apply(handlers)
                .whenComplete((ignored, error) -> handlers.close());
        } catch (RuntimeException error) {
            handlers.close();
            throw error;
        }
    }
}
