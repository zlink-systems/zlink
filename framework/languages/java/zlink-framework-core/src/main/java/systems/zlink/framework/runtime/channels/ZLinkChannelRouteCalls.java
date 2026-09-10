package systems.zlink.framework.runtime.channels;
import java.util.concurrent.atomic.AtomicBoolean;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.TimeoutException;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
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
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageContext;
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
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchFailure;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;


final class RouteSendCall implements ZLinkSendCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final ZLinkChannelSocketRegistry sockets;
    private final RoutingId target;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    RouteSendCall(
        ZLinkChannelCallRuntime runtime, String channelName,
        ZLinkChannelSocketRegistry sockets, RoutingId target, Message payload,
        Optional<String> packetName, String contentType, ZLinkApplicationMetadata metadata) {
        this(runtime, channelName, sockets, target, payload, packetName, contentType,
            metadata, new AtomicBoolean());
    }

    private RouteSendCall(
        ZLinkChannelCallRuntime runtime, String channelName,
        ZLinkChannelSocketRegistry sockets, RoutingId target, Message payload,
        Optional<String> packetName, String contentType, ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.channelName = channelName;
        this.sockets = sockets;
        this.target = target;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkSendCall packetName(String name) {
        return new RouteSendCall(runtime, channelName, sockets, target, payload,
            Optional.of(name), contentType, metadata, submitGate);
    }

    @Override
    public ZLinkSendCall metadata(String key, String value) {
        return new RouteSendCall(runtime, channelName, sockets, target, payload,
            packetName, contentType,
            (metadata == null ? ZLinkApplicationMetadata.empty() : metadata).with(key, value), submitGate);
    }

    @Override
    public ZLinkSendCall metadata(Map<String, String> values) {
        return new RouteSendCall(runtime, channelName, sockets, target, payload,
            packetName, contentType,
            (metadata == null ? ZLinkApplicationMetadata.empty() : metadata).withAll(values), submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
            return sockets.submitToNode(channelName, null, null,
                (router, timeout) -> submitRouter(router),
                (node, timeout) -> submitNode(node));
        } catch (RuntimeException | Error failure) {
            payload.close();
            throw failure;
        }
    }

    private CompletionStage<Void> submitRouter(ZLinkBackendRouterSocket router) {
        if (metadata != null) {
            throw new UnsupportedOperationException("send metadata is not available");
        }
        traceSent();
        List<Message> parts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_COMMAND,
            "", packetName, payload, contentType, Map.of());
        try {
            return ZLinkOneWayCalls.adaptOneWay(router.send(target, parts))
                .whenComplete((ignored, failure) -> parts.forEach(Message::close));
        } catch (RuntimeException | Error failure) {
            parts.forEach(Message::close);
            throw failure;
        }
    }

    private CompletionStage<Void> submitNode(ZLinkInternalSpotNode node) {
        ZLinkApplicationMetadata metadata = this.metadata == null
            ? ZLinkApplicationMetadata.empty() : this.metadata;
        traceSent();
        List<Message> sendParts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_COMMAND,
            "", packetName, payload, contentType, metadata.values());
        if (node.routingId().equals(target)) {
            return submitLocal(node, target, metadata.encode(), sendParts)
                .thenCompose(ZLinkOneWayCalls::oneWayStatus)
                .whenComplete((ignored, failure) ->
                    sendParts.forEach(Message::close));
        }
        Optional<Integer> classified =
            node.classifyNodeSendTarget(target);
        if (classified.isPresent()) {
            sendParts.forEach(Message::close);
            return ZLinkOneWayCalls.oneWayStatus(classified.orElseThrow());
        }
        try {
            return ZLinkOneWayCalls.adaptOneWay(
                    node.sendToNode(target, metadata.encode(), sendParts))
                .whenComplete((ignored, failure) ->
                    sendParts.forEach(Message::close));
        } catch (RuntimeException | Error failure) {
            sendParts.forEach(Message::close);
            throw failure;
        }
    }

    private void traceSent() {
        ZLinkMessageFlowTracer.TracePoint sent =
            runtime.flow().begin(ZLinkMessageFlowOutcome.SENT);
        if (sent != null) {
            sent.trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                ZLinkDispatchMessageKind.SEND,
                packetName.orElse(null), null, null, null, target.toString(), null, null, null));
        }
    }

    private static CompletionStage<Integer> submitLocal(
        ZLinkInternalSpotNode node,
        RoutingId target,
        byte[] metadata,
        List<Message> parts) {
        return node.submitLocalNodeSend(target, metadata, parts)
            .orElseGet(() -> CompletableFuture.completedFuture(
                ZLinkOneWayCalls.ROUTE_NOT_CONNECTED));
    }
}


final class RouteRequestCall implements ZLinkRequestCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final ZLinkChannelSocketRegistry sockets;
    private final Duration defaultTimeout;
    private final RoutingId target;
    private final Message payload;
    private final Optional<String> packetName;
    private final Duration timeout;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    RouteRequestCall(
        ZLinkChannelCallRuntime runtime, String channelName,
        ZLinkChannelSocketRegistry sockets, Duration defaultTimeout, RoutingId target,
        Message payload, Optional<String> packetName, Duration timeout,
        String contentType, ZLinkApplicationMetadata metadata) {
        this(runtime, channelName, sockets, defaultTimeout, target, payload, packetName,
            timeout, contentType, metadata, new AtomicBoolean());
    }

    private RouteRequestCall(
        ZLinkChannelCallRuntime runtime, String channelName,
        ZLinkChannelSocketRegistry sockets, Duration defaultTimeout, RoutingId target,
        Message payload, Optional<String> packetName, Duration timeout,
        String contentType, ZLinkApplicationMetadata metadata, AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.channelName = channelName;
        this.sockets = sockets;
        this.defaultTimeout = defaultTimeout;
        this.target = target;
        this.payload = payload;
        this.packetName = packetName;
        this.timeout = timeout;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkRequestCall packetName(String name) {
        return new RouteRequestCall(runtime, channelName, sockets, defaultTimeout, target,
            payload, Optional.of(name), timeout, contentType, metadata, submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(String key, String value) {
        return new RouteRequestCall(runtime, channelName, sockets, defaultTimeout, target,
            payload, packetName, timeout, contentType,
            (metadata == null ? ZLinkApplicationMetadata.empty() : metadata).with(key, value), submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(Map<String, String> values) {
        return new RouteRequestCall(runtime, channelName, sockets, defaultTimeout, target,
            payload, packetName, timeout, contentType,
            (metadata == null ? ZLinkApplicationMetadata.empty() : metadata).withAll(values), submitGate);
    }

    @Override
    public ZLinkRequestCall timeout(Duration value) {
        Objects.requireNonNull(value, "timeout");
        return new RouteRequestCall(runtime, channelName, sockets, defaultTimeout, target,
            payload, packetName, value, contentType, metadata, submitGate);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        CompletionStage<TReply> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
            try {
                return ZLinkSerialExecutionQueue.manageCurrent(sockets.submitToNode(
                    channelName, timeout, defaultTimeout,
                    (router, effectiveTimeout) -> submitRouter(router, effectiveTimeout, replyType),
                    (node, effectiveTimeout) -> submitNode(node, effectiveTimeout, replyType)));
            } catch (ZLinkConfigurationException failure) {
                payload.close();
                throw failure;
            } catch (RuntimeException failure) {
                payload.close();
                CompletableFuture<TReply> result = requestResult();
                result.completeExceptionally(failure);
                return ZLinkSerialExecutionQueue.manageCurrent(result);
            } catch (Error failure) {
                payload.close();
                throw failure;
            }
        }
    }

    private <TReply> CompletionStage<TReply> submitRouter(
        ZLinkBackendRouterSocket router, Duration timeout, Class<TReply> replyType) {
        if (metadata != null) {
            throw new UnsupportedOperationException("request metadata is not available");
        }
        CompletableFuture<TReply> result = requestResult();
        var operationId = ZLinkServiceOperationIds.next();
        List<Message> requestParts = requestParts(operationId, Map.of());
        runtime.requestRoute(operationId, router, target, requestParts, timeout)
            .whenComplete((reply, failure) -> {
                requestParts.forEach(Message::close);
                if (failure != null) {
                    result.completeExceptionally(
                        ZLinkChannelCallRuntime.requestFailure(failure));
                    return;
                }
                if (result.isDone()) {
                    reply.close();
                    return;
                }
                try {
                    runtime.completeReply(reply, replyType, result);
                } catch (RuntimeException ex) {
                    result.completeExceptionally(ex);
                } finally {
                    reply.close();
                }
            });
        return result;
    }

    private <TReply> CompletionStage<TReply> submitNode(
        ZLinkInternalSpotNode node, Duration timeout, Class<TReply> replyType) {
        ZLinkApplicationMetadata metadata = this.metadata == null
            ? ZLinkApplicationMetadata.empty() : this.metadata;
        CompletableFuture<TReply> result = requestResult();
        var operationId = ZLinkServiceOperationIds.next();
        List<Message> requestParts = requestParts(operationId, metadata.values());
        try {
            Optional<Integer> classified =
                node.classifyNodeSendTarget(target);
            if (classified.isPresent()) {
                int status = classified.orElseThrow();
                result.completeExceptionally(switch (status) {
                    case ZLinkOneWayCalls.TARGET_NOT_FOUND ->
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NOT_FOUND,
                            "RouteMesh node request target was not found: "
                                + target);
                    case ZLinkOneWayCalls.ROUTE_NOT_CONNECTED ->
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            "RouteMesh node request route is not connected: "
                                + target);
                    default -> new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                        "RouteMesh node request target classification failed: "
                            + status);
                });
                return result;
            }
            runtime.requestNode(
                    operationId, node, target,
                    metadata.encode(),
                    requestParts,
                    timeout)
                .whenComplete((reply, failure) -> {
                    if (failure != null) {
                        result.completeExceptionally(ZLinkChannelCallRuntime.requestFailure(failure));
                        return;
                    }
                    try {
                        runtime.completeReply(reply, replyType, result);
                    } catch (RuntimeException error) {
                        result.completeExceptionally(error);
                    } finally {
                        reply.close();
                    }
                });
        } catch (RuntimeException error) {
            result.completeExceptionally(error);
        } finally {
            requestParts.forEach(Message::close);
        }
        return result;
    }

    private <TReply> CompletableFuture<TReply> requestResult() {
        CompletableFuture<TReply> result = new CompletableFuture<>();
        result.whenComplete((ignored, error) -> {
            ZLinkMessageFlowTracer.TerminalTracePoint terminal =
                runtime.flow().beginRequestTerminal(error, result);
            if (terminal != null) {
                terminal.trace(new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.REPLY_RECEIVED,
                    ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                    ZLinkDispatchMessageKind.REQUEST,
                    packetName.orElse(null), channelName, null, null,
                    target.toString(), null, null, null));
            }
        });
        return result;
    }

    private List<Message> requestParts(java.util.UUID operationId, Map<String, String> metadata) {
        List<Message> requestParts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_REQUEST,
            channelName, packetName, payload, contentType, metadata, operationId);
        ZLinkMessageFlowTracer.TracePoint sent =
            runtime.flow().begin(ZLinkMessageFlowOutcome.SENT);
        if (sent != null) {
            sent.trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                ZLinkDispatchMessageKind.REQUEST,
                packetName.orElse(null), channelName, null, null,
                target.toString(), null, null, null));
        }
        return requestParts;
    }

    @Override
    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
        return systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectYield("MeshNode request");
    }
}

final class ChannelSendCall implements ZLinkSendCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final ZLinkChannelSocketRegistry sockets;
    private final Duration defaultTimeout;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    ChannelSendCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkChannelSocketRegistry sockets,
        Duration defaultTimeout,
        Message payload,
        Optional<String> packetName) {
        this(
            runtime,
            channelName,
            sockets,
            defaultTimeout,
            payload,
            packetName,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
            null);
    }

    ChannelSendCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkChannelSocketRegistry sockets,
        Duration defaultTimeout,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this(runtime, channelName, sockets, defaultTimeout, payload, packetName, contentType, metadata,
            new AtomicBoolean());
    }

    private ChannelSendCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkChannelSocketRegistry sockets,
        Duration defaultTimeout,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.channelName = channelName;
        this.sockets = sockets;
        this.defaultTimeout = defaultTimeout;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkSendCall packetName(String name) {
        return new ChannelSendCall(
            runtime, channelName, sockets, defaultTimeout, payload, Optional.of(name), contentType, metadata,
            submitGate);
    }

    @Override
    public ZLinkSendCall metadata(String key, String value) {
        return new ChannelSendCall(
            runtime, channelName, sockets, defaultTimeout, payload, packetName, contentType,
            (metadata == null ? ZLinkApplicationMetadata.empty() : metadata).with(key, value), submitGate);
    }

    @Override
    public ZLinkSendCall metadata(Map<String, String> values) {
        return new ChannelSendCall(
            runtime, channelName, sockets, defaultTimeout, payload, packetName, contentType,
            (metadata == null ? ZLinkApplicationMetadata.empty() : metadata).withAll(values), submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
            return sockets.submitToChannel(channelName, null, defaultTimeout, metadata != null,
                (client, remaining) -> submitClient(client),
                (node, timeout) -> submitMesh(node));
        } catch (RuntimeException | Error failure) {
            payload.close();
            throw failure;
        }
    }

    private CompletionStage<Void> submitClient(ZLinkBackendDealerSocket client) {
        ZLinkMessageFlowTracer.TracePoint sent =
            runtime.flow().begin(ZLinkMessageFlowOutcome.SENT);
        if (sent != null) {
            sent.trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchErrorSurface.CHANNEL,
                ZLinkDispatchMessageKind.SEND,
                packetName.orElse(null), null, null, null, null, null, null, null));
        }
        List<Message> parts = ZLinkChannelCallRuntime.parts(
            packetName, payload, contentType);
        try {
            return ZLinkOneWayCalls.adaptOneWay(client.send(parts))
                .whenComplete((ignored, failure) -> parts.forEach(Message::close));
        } catch (RuntimeException | Error failure) {
            parts.forEach(Message::close);
            throw failure;
        }
    }

    private CompletionStage<Void> submitMesh(ZLinkInternalSpotNode node) {
        ZLinkApplicationMetadata metadata = this.metadata == null
            ? ZLinkApplicationMetadata.empty() : this.metadata;
        List<Message> parts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_COMMAND,
            channelName, packetName, payload, contentType, metadata.values());
        try {
            return ZLinkOneWayCalls.adaptOneWay(
                    node.sendToChannel(channelName, metadata.encode(), parts))
                .whenComplete((ignored, failure) ->
                    parts.forEach(Message::close));
        } catch (RuntimeException | Error failure) {
            parts.forEach(Message::close);
            throw failure;
        }
    }
}

final class ChannelRequestCall implements ZLinkRequestCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final ZLinkChannelSocketRegistry sockets;
    private final Duration defaultTimeout;
    private final Message payload;
    private final Optional<String> packetName;
    private final Duration timeout;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    ChannelRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkChannelSocketRegistry sockets,
        Duration defaultTimeout,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        this(
            runtime,
            channelName,
            sockets,
            defaultTimeout,
            payload,
            packetName,
            timeout,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
            null);
    }

    ChannelRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkChannelSocketRegistry sockets,
        Duration defaultTimeout,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this(runtime, channelName, sockets, defaultTimeout, payload, packetName, timeout, contentType,
            metadata, new AtomicBoolean());
    }

    private ChannelRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkChannelSocketRegistry sockets,
        Duration defaultTimeout,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.channelName = channelName;
        this.sockets = sockets;
        this.defaultTimeout = defaultTimeout;
        this.payload = payload;
        this.packetName = packetName;
        this.timeout = timeout;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkRequestCall packetName(String name) {
        return new ChannelRequestCall(
            runtime, channelName, sockets, defaultTimeout, payload, Optional.of(name), timeout,
            contentType, metadata, submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(String key, String value) {
        return new ChannelRequestCall(
            runtime, channelName, sockets, defaultTimeout, payload, packetName, timeout,
            contentType, (metadata == null ? ZLinkApplicationMetadata.empty() : metadata).with(key, value), submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(Map<String, String> values) {
        return new ChannelRequestCall(
            runtime, channelName, sockets, defaultTimeout, payload, packetName, timeout,
            contentType, (metadata == null ? ZLinkApplicationMetadata.empty() : metadata).withAll(values), submitGate);
    }

    @Override
    public ZLinkRequestCall timeout(Duration value) {
        java.util.Objects.requireNonNull(value, "timeout");
        return new ChannelRequestCall(
            runtime, channelName, sockets, defaultTimeout, payload, packetName, value, contentType, metadata,
            submitGate);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        CompletionStage<TReply> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        long started = runtime.nanoTime();
        try (var flowScope = runtime.enterApplicationFlow()) {
            try {
                return ZLinkSerialExecutionQueue.manageCurrent(
                    sockets.submitToChannel(channelName, timeout, defaultTimeout, metadata != null,
                        (client, remaining) -> submitClient(client, remaining, replyType, started),
                        (node, effectiveTimeout) -> submitMesh(node, effectiveTimeout, replyType, started)));
            } catch (ZLinkConfigurationException failure) {
                payload.close();
                throw failure;
            } catch (RuntimeException failure) {
                payload.close();
                CompletableFuture<TReply> result = requestResult(ZLinkDispatchErrorSurface.CHANNEL, started);
                result.completeExceptionally(failure);
                return ZLinkSerialExecutionQueue.manageCurrent(result);
            } catch (Error failure) {
                payload.close();
                throw failure;
            }
        }
    }

    private <TReply> CompletionStage<TReply> submitClient(
        ZLinkBackendDealerSocket target, Duration timeout, Class<TReply> replyType, long started) {
        CompletableFuture<TReply> result = requestResult(ZLinkDispatchErrorSurface.CHANNEL, started);
        String reqPacket = packetName.orElse(null);
        List<Message> requestParts = ZLinkChannelCallRuntime.parts(
            packetName, payload, contentType);
        result.whenComplete((ignored, error) -> requestParts.forEach(Message::close));
        try {
            Duration remaining = timeout;
            if (remaining.isZero() || remaining.isNegative()) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    "channel request deadline expired during ready wait",
                    new TimeoutException("channel request deadline expired"));
            }
            ZLinkMessageFlowTracer.TracePoint sent =
                runtime.flow().begin(ZLinkMessageFlowOutcome.SENT);
            if (sent != null) {
                sent.trace(new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.SENT,
                    ZLinkDispatchErrorSurface.CHANNEL,
                    ZLinkDispatchMessageKind.REQUEST,
                    reqPacket, null, null, null, null, null, null, null));
            }
            runtime.requestClient(target, requestParts, remaining)
                .whenComplete((reply, failure) -> {
                    if (failure != null) {
                        result.completeExceptionally(
                            ZLinkChannelCallRuntime.requestFailure(failure));
                        return;
                    }
                    if (result.isDone()) {
                        reply.close();
                        return;
                    }
                    try {
                        runtime.completeReply(reply, replyType, result);
                    } catch (RuntimeException ex) {
                        result.completeExceptionally(ex);
                    } finally {
                        reply.close();
                    }
                });
        } catch (RuntimeException failure) {
            result.completeExceptionally(failure);
        }
        return result;
    }

    private <TReply> CompletionStage<TReply> submitMesh(
        ZLinkInternalSpotNode node, Duration timeout, Class<TReply> replyType, long started) {
        ZLinkApplicationMetadata metadata = this.metadata == null
            ? ZLinkApplicationMetadata.empty() : this.metadata;
        CompletableFuture<TReply> result = requestResult(ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL, started);
        var operationId = ZLinkServiceOperationIds.next();
        List<Message> parts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_REQUEST,
            channelName,
            packetName,
            payload,
            contentType,
            metadata.values(), operationId);
        runtime.requestChannel(
                operationId, node, channelName,
                metadata.encode(),
                parts,
                timeout)
            .whenComplete((reply, failure) -> {
                parts.forEach(Message::close);
                if (failure != null) {
                    result.completeExceptionally(ZLinkChannelCallRuntime.requestFailure(failure));
                    return;
                }
                try {
                    runtime.completeReply(reply, replyType, result);
                } catch (RuntimeException error) {
                    result.completeExceptionally(error);
                } finally {
                    reply.close();
                }
            });
        return result;
    }

    private <TReply> CompletableFuture<TReply> requestResult(
        ZLinkDispatchErrorSurface surface, long started) {
        CompletableFuture<TReply> result = new CompletableFuture<>();
        result.whenComplete((ignored, error) -> {
            ZLinkMessageFlowTracer.TerminalTracePoint terminal =
                runtime.flow().beginRequestTerminal(error, result);
            if (terminal != null) {
                terminal.trace(new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.REPLY_RECEIVED,
                    surface,
                    ZLinkDispatchMessageKind.REQUEST,
                    packetName.orElse(null),
                    surface == ZLinkDispatchErrorSurface.CHANNEL ? null : channelName, null, null,
                    null, null, null, null));
            }
        });

        if (surface == ZLinkDispatchErrorSurface.CHANNEL && ZLinkRuntimeMetrics.enabled()) {
            ZLinkRequestMetricTags metricTags = ZLinkRequestMetricTags.forChannel(channelName);
            ZLinkRuntimeMetrics.add(
                "zlink.mesh_node.requests.inflight", 1, metricTags.request);
            result.whenComplete((ignored, error) -> {
                ZLinkRuntimeMetrics.add(
                    "zlink.mesh_node.requests.inflight", -1, metricTags.request);
                boolean timedOut = error instanceof TimeoutException
                    || (error != null && error.getCause() instanceof TimeoutException);
                ZLinkRuntimeMetrics.record("zlink.mesh_node.request.duration",
                    Duration.ofNanos(runtime.nanoTime() - started),
                    metricTags.duration(timedOut, error != null));
                if (timedOut) {
                    ZLinkRuntimeMetrics.increment(
                        "zlink.mesh_node.request.timeouts", metricTags.request);
                }
            });
        }
        return result;
    }

    @Override
    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.requireYieldAllowed("Channel request");
        return ZLinkSerialExecutionQueue
            .yieldCurrent(submit(replyType));
    }

}
