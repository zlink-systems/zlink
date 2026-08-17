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
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
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
    private final ZLinkBackendRouterSocket router;
    private final RoutingId target;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;

    RouteSendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendRouterSocket router,
        RoutingId target,
        Message payload,
        Optional<String> packetName) {
        this(runtime, router, target, payload, packetName,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    RouteSendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendRouterSocket router,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        this(runtime, router, target, payload, packetName, contentType,
            new AtomicBoolean());
    }

    private RouteSendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendRouterSocket router,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        String contentType,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.router = router;
        this.target = target;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
    }

    RouteSendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendRouterSocket router,
        RoutingId target,
        Message payload) {
        this(runtime, router, target, payload, Optional.empty());
    }

    public ZLinkSendCall packetName(String packetName) {
        return new RouteSendCall(
            runtime, router, target, payload, Optional.of(packetName), contentType,
            submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
        ZLinkMessageFlowTracer.TracePoint sent =
            runtime.flow().begin(ZLinkMessageFlowOutcome.SENT);
        if (sent != null) {
            sent.trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                ZLinkDispatchMessageKind.SEND,
                packetName.orElse(null), null, null, null, target.toString(), null, null, null));
        }
        List<Message> sendParts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_COMMAND,
            "", packetName, payload, contentType, Map.of());
        return ZLinkOneWayCalls.adaptOneWay(router.send(target, sendParts))
            .whenComplete((ignored, failure) ->
                sendParts.forEach(Message::close));
        }
    }
}

final class RouteRequestCall implements ZLinkRequestCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final ZLinkBackendRouterSocket router;
    private final RoutingId target;
    private final Message payload;
    private final Optional<String> packetName;
    private final Duration timeout;
    private final String contentType;

    RouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkBackendRouterSocket router,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        this(runtime, channelName, router, target, payload, packetName, timeout,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    RouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkBackendRouterSocket router,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType) {
        this(runtime, channelName, router, target, payload, packetName, timeout,
            contentType, new AtomicBoolean());
    }

    private RouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkBackendRouterSocket router,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.channelName = channelName;
        this.router = router;
        this.target = target;
        this.payload = payload;
        this.packetName = packetName;
        this.timeout = timeout;
        this.contentType = contentType;
    }

    public ZLinkRequestCall packetName(String packetName) {
        return new RouteRequestCall(
            runtime,
            channelName,
            router,
            target,
            payload,
            Optional.of(packetName),
            timeout,
            contentType,
            submitGate);
    }

    @Override
    public ZLinkRequestCall timeout(Duration timeout) {
        return new RouteRequestCall(
            runtime, channelName, router, target, payload, packetName, timeout, contentType,
            submitGate);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        CompletionStage<TReply> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
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
        runtime.track(result, timeout);
        List<Message> requestParts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_REQUEST,
            channelName, packetName, payload, contentType, Map.of());
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
        runtime.requestRoute(router, target, requestParts, timeout)
            .whenComplete((reply, failure) -> {
                requestParts.forEach(Message::close);
                if (failure != null) {
                    result.completeExceptionally(
                        ZLinkChannelCallRuntime.unwrap(failure));
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
        return ZLinkAsyncSerialQueue.manageCurrent(result);
        }
    }

    @Override
    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
        return systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectYield("MeshNode request");
    }

}

final class MeshNodeRouteSendCall implements ZLinkSendCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final ZLinkInternalSpotNode node;
    private final RoutingId target;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    MeshNodeRouteSendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkInternalSpotNode node,
        RoutingId target,
        Message payload,
        Optional<String> packetName) {
        this(runtime, node, target, payload, packetName,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
            ZLinkApplicationMetadata.empty());
    }

    MeshNodeRouteSendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkInternalSpotNode node,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this(runtime, node, target, payload, packetName, contentType, metadata,
            new AtomicBoolean());
    }

    private MeshNodeRouteSendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkInternalSpotNode node,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.node = node;
        this.target = target;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkSendCall packetName(String name) {
        return new MeshNodeRouteSendCall(
            runtime, node, target, payload, Optional.of(name), contentType, metadata,
            submitGate);
    }

    @Override
    public ZLinkSendCall metadata(String key, String value) {
        return new MeshNodeRouteSendCall(
            runtime, node, target, payload, packetName, contentType,
            metadata.with(key, value), submitGate);
    }

    @Override
    public ZLinkSendCall metadata(Map<String, String> values) {
        return new MeshNodeRouteSendCall(
            runtime, node, target, payload, packetName, contentType,
            metadata.withAll(values), submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
        ZLinkMessageFlowTracer.TracePoint sent =
            runtime.flow().begin(ZLinkMessageFlowOutcome.SENT);
        if (sent != null) {
            sent.trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                ZLinkDispatchMessageKind.SEND,
                packetName.orElse(null), null, null, null, target.toString(), null, null, null));
        }
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
        return ZLinkOneWayCalls.adaptOneWay(
                node.sendToNode(target, metadata.encode(), sendParts))
            .whenComplete((ignored, failure) ->
                sendParts.forEach(Message::close));
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

final class MeshChannelRouteSendCall implements ZLinkSendCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final ZLinkInternalSpotNode node;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    MeshChannelRouteSendCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        Message payload,
        Optional<String> packetName) {
        this(
            runtime,
            channelName,
            node,
            payload,
            packetName,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
            ZLinkApplicationMetadata.empty());
    }

    MeshChannelRouteSendCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this(runtime, channelName, node, payload, packetName, contentType, metadata,
            new AtomicBoolean());
    }

    private MeshChannelRouteSendCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.channelName = channelName;
        this.node = node;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkSendCall packetName(String name) {
        return new MeshChannelRouteSendCall(
            runtime, channelName, node, payload, Optional.of(name), contentType, metadata,
            submitGate);
    }

    @Override
    public ZLinkSendCall metadata(String key, String value) {
        return new MeshChannelRouteSendCall(
            runtime, channelName, node, payload, packetName, contentType,
            metadata.with(key, value), submitGate);
    }

    @Override
    public ZLinkSendCall metadata(Map<String, String> values) {
        return new MeshChannelRouteSendCall(
            runtime, channelName, node, payload, packetName, contentType,
            metadata.withAll(values), submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
        Optional<Integer> classified =
            node.classifyChannelTarget(channelName);
        if (classified.isPresent()) {
            payload.close();
            ZLinkChannelRuntime.trace(ZLinkChannelRuntime.traceEnabled() ?
                "route-channel target=none channel=" + channelName
                    + " status=" + classified.orElseThrow() : null);
            return ZLinkOneWayCalls.oneWayStatus(classified.orElseThrow());
        }
        List<Message> parts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_COMMAND,
            channelName, packetName, payload, contentType, metadata.values());
        return ZLinkOneWayCalls.adaptOneWay(
                node.sendToChannel(channelName, metadata.encode(), parts))
            .whenComplete((ignored, failure) ->
                parts.forEach(Message::close));
        }
    }
}

final class MeshChannelRouteRequestCall implements ZLinkRequestCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final ZLinkInternalSpotNode node;
    private final Message payload;
    private final Optional<String> packetName;
    private final Duration timeout;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    MeshChannelRouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        this(
            runtime,
            channelName,
            node,
            payload,
            packetName,
            timeout,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
            ZLinkApplicationMetadata.empty());
    }

    MeshChannelRouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this(runtime, channelName, node, payload, packetName, timeout, contentType,
            metadata, new AtomicBoolean());
    }

    private MeshChannelRouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.channelName = channelName;
        this.node = node;
        this.payload = payload;
        this.packetName = packetName;
        this.timeout = timeout;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkRequestCall packetName(String name) {
        return new MeshChannelRouteRequestCall(
            runtime, channelName, node, payload, Optional.of(name), timeout,
            contentType, metadata, submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(String key, String value) {
        return new MeshChannelRouteRequestCall(
            runtime, channelName, node, payload, packetName, timeout,
            contentType, metadata.with(key, value), submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(Map<String, String> values) {
        return new MeshChannelRouteRequestCall(
            runtime, channelName, node, payload, packetName, timeout,
            contentType, metadata.withAll(values), submitGate);
    }

    @Override
    public ZLinkRequestCall timeout(Duration value) {
        return new MeshChannelRouteRequestCall(
            runtime, channelName, node, payload, packetName, value, contentType, metadata,
            submitGate);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        CompletionStage<TReply> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
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
                    null, null, null, null));
            }
        });
        runtime.track(result, timeout);
        Optional<Integer> classified =
            node.classifyChannelTarget(channelName);
        if (classified.isPresent()) {
            payload.close();
            ZLinkChannelRuntime.trace(ZLinkChannelRuntime.traceEnabled() ?
                "route-channel target=none channel=" + channelName
                    + " status=" + classified.orElseThrow() : null);
            result.completeExceptionally(
                ZLinkOneWayCalls.failureForStatus(classified.orElseThrow()));
            return ZLinkAsyncSerialQueue
                .manageCurrent(result);
        }
        result.whenComplete((ignored, failure) -> payload.close());
        List<Message> parts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_REQUEST,
            channelName,
            packetName,
            Message.from(payload),
            contentType,
            metadata.values());
        node.requestToChannel(
                channelName,
                metadata.encode(),
                parts,
                timeout)
            .whenComplete((reply, failure) -> {
                parts.forEach(Message::close);
                if (failure != null) {
                    result.completeExceptionally(failure);
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
        return ZLinkAsyncSerialQueue.manageCurrent(result);
        }
    }

    @Override
    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.requireYieldAllowed("Channel request");
        return ZLinkAsyncSerialQueue
            .yieldCurrent(submit(replyType));
    }

}

final class MeshNodeRouteRequestCall implements ZLinkRequestCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final ZLinkInternalSpotNode node;
    private final RoutingId target;
    private final Message payload;
    private final Optional<String> packetName;
    private final Duration timeout;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    MeshNodeRouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        this(
            runtime,
            channelName,
            node,
            target,
            payload,
            packetName,
            timeout,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
            ZLinkApplicationMetadata.empty());
    }

    MeshNodeRouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this(runtime, channelName, node, target, payload, packetName, timeout, contentType,
            metadata, new AtomicBoolean());
    }

    private MeshNodeRouteRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        ZLinkInternalSpotNode node,
        RoutingId target,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.channelName = channelName;
        this.node = node;
        this.target = target;
        this.payload = payload;
        this.packetName = packetName;
        this.timeout = timeout;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkRequestCall packetName(String name) {
        return new MeshNodeRouteRequestCall(
            runtime, channelName, node, target, payload, Optional.of(name), timeout,
            contentType, metadata, submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(String key, String value) {
        return new MeshNodeRouteRequestCall(
            runtime, channelName, node, target, payload, packetName, timeout,
            contentType, metadata.with(key, value), submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(Map<String, String> values) {
        return new MeshNodeRouteRequestCall(
            runtime, channelName, node, target, payload, packetName, timeout,
            contentType, metadata.withAll(values), submitGate);
    }

    @Override
    public ZLinkRequestCall timeout(Duration value) {
        return new MeshNodeRouteRequestCall(
            runtime, channelName, node, target, payload, packetName, value,
            contentType, metadata, submitGate);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        CompletionStage<TReply> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
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
        runtime.track(result, timeout);
        List<Message> requestParts = ZLinkChannelCallRuntime.envelopeParts(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_REQUEST,
            channelName, packetName, payload, contentType, metadata.values());
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
                return systems.zlink.framework.execution
                    .ZLinkAsyncSerialQueue.manageCurrent(result);
            }
            node.requestToNode(
                    target,
                    metadata.encode(),
                    requestParts,
                    timeout)
                .whenComplete((reply, failure) -> {
                    if (failure != null) {
                        result.completeExceptionally(failure);
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
        return ZLinkAsyncSerialQueue.manageCurrent(result);
        }
    }

    @Override
    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
        return systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectYield("MeshNode request");
    }
}
