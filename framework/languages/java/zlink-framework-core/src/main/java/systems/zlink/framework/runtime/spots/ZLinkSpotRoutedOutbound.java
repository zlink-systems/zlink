package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;


final class ZLinkSpotRoutedOutbound {
    private final ZLinkChannelRuntime channels;
    private final ZLinkSpotRouteMessages messages;
    private final ZLinkSpotDirectOutbound directOutbound;
    private final ZLinkMessageFlowTracer flow;
    private final Function<String, ZLinkInternalSpotNode> localRouterNode;

    ZLinkSpotRoutedOutbound(
        ZLinkChannelRuntime channels,
        ZLinkSpotRouteMessages messages,
        ZLinkSpotDirectOutbound directOutbound,
        ZLinkMessageFlowTracer flow,
        Function<String, ZLinkInternalSpotNode> localRouterNode) {
        this.channels = channels;
        this.messages = messages;
        this.directOutbound = directOutbound;
        this.flow = flow;
        this.localRouterNode = localRouterNode;
    }

    ZLinkSendCall send(
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName) {
        return send(
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            null);
    }

    ZLinkSendCall send(
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        return new ZLinkSpotRoutedSendCall(
            this,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType);
    }

    ZLinkRequestCall request(
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        return request(
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            null,
            timeout);
    }

    ZLinkRequestCall request(
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Duration timeout) {
        return new ZLinkSpotRoutedRequestCall(
            this,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout);
    }

    CompletionStage<Void> submitSend(
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        trace(
            ZLinkMessageFlowOutcome.SENT,
            ZLinkDispatchMessageKind.SEND,
            packetName,
            routerChannelId,
            spotId);
        ZLinkInternalSpotNode routerNode = localRouterNode.apply(routerChannelId);
        if (routerNode != null) {
            return directOutbound.send(
                routerNode.entrySpot(),
                targetNodeRid,
                spotId,
                spotGeneration,
                payload,
                packetName,
                contentType)
                .metadata(metadata.values())
                .submit();
        }
        if (!metadata.values().isEmpty()) {
            throw new UnsupportedOperationException(
                "legacy routed Spot transport does not support application metadata");
        }
        List<Message> parts = messages.encode(packetName, payload, contentType);
        try {
            return ZLinkOneWayCalls.adaptOneWay(channels.sendToSpotViaRouterChannel(
                routerChannelId,
                targetNodeRid,
                spotId,
                spotGeneration,
                parts).whenComplete((ignored, error) -> {
                    parts.forEach(Message::close);
                    if (error != null) {
                        java.util.logging.Logger.getLogger(ZLinkSpotRoutedOutbound.class.getName())
                            .log(java.util.logging.Level.SEVERE, "one-way routed SPOT submission failed", error);
                    }
                }));
        } catch (RuntimeException error) {
            parts.forEach(Message::close);
            throw error;
        }
    }

    <TReply> CompletionStage<TReply> submitRequest(
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata,
        Duration timeout,
        Class<TReply> replyType) {
        trace(
            ZLinkMessageFlowOutcome.SENT,
            ZLinkDispatchMessageKind.REQUEST,
            packetName,
            routerChannelId,
            spotId);
        ZLinkInternalSpotNode routerNode = localRouterNode.apply(routerChannelId);
        if (routerNode != null) {
            return directOutbound.request(
                routerNode.entrySpot(),
                targetNodeRid,
                spotId,
                spotGeneration,
                payload,
                packetName,
                contentType,
                timeout)
                .metadata(metadata.values())
                .submit(replyType);
        }
        if (!metadata.values().isEmpty()) {
            throw new UnsupportedOperationException(
                "legacy routed Spot transport does not support application metadata");
        }
        List<Message> parts = messages.encode(packetName, payload, contentType);
        try {
                return channels.requestToSpotViaRouterChannel(
                    routerChannelId,
                    targetNodeRid,
                    spotId,
                    spotGeneration,
                    parts,
                    timeout)
                .thenApply(replyParts -> {
                    trace(
                        ZLinkMessageFlowOutcome.REPLY_RECEIVED,
                        ZLinkDispatchMessageKind.RESPONSE,
                        packetName,
                        routerChannelId,
                        spotId);
                    try {
                        return messages.decodeReply(replyParts, replyType);
                    } finally {
                        replyParts.forEach(Message::close);
                    }
                });
        } finally {
            parts.forEach(Message::close);
        }
    }

    private void trace(
        ZLinkMessageFlowOutcome outcome,
        ZLinkDispatchMessageKind kind,
        Optional<String> packetName,
        String routerChannelId,
        String spotId) {
        if (!flow.enabled(outcome)) {
            return;
        }
        flow.trace(new ZLinkMessageFlowEvent(
            outcome,
            ZLinkDispatchErrorSurface.SPOT_ROUTE,
            kind,
            packetName.orElse(null),
            routerChannelId,
            null,
            null,
            null,
            spotId.toString(),
            null,
            null));
    }
}

final class ZLinkSpotRoutedSendCall implements ZLinkSendCall {
    private final java.util.concurrent.atomic.AtomicBoolean submitGate =
        new java.util.concurrent.atomic.AtomicBoolean();
    private final ZLinkSpotRoutedOutbound outbound;
    private final String routerChannelId;
    private final RoutingId targetNodeRid;
    private final String spotId;
    private final long spotGeneration;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    ZLinkSpotRoutedSendCall(
        ZLinkSpotRoutedOutbound outbound,
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName) {
        this(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            null,
            ZLinkApplicationMetadata.empty());
    }

    ZLinkSpotRoutedSendCall(
        ZLinkSpotRoutedOutbound outbound,
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        this(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            ZLinkApplicationMetadata.empty());
    }

    private ZLinkSpotRoutedSendCall(
        ZLinkSpotRoutedOutbound outbound,
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this.outbound = outbound;
        this.routerChannelId = routerChannelId;
        this.targetNodeRid = targetNodeRid;
        this.spotId = spotId;
        this.spotGeneration = spotGeneration;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkSendCall packetName(String packetName) {
        return new ZLinkSpotRoutedSendCall(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            Optional.of(packetName),
            contentType,
            metadata);
    }

    @Override
    public ZLinkSendCall metadata(String key, String value) {
        return new ZLinkSpotRoutedSendCall(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            metadata.with(key, value));
    }

    @Override
    public ZLinkSendCall metadata(Map<String, String> values) {
        return new ZLinkSpotRoutedSendCall(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            metadata.withAll(values));
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        return outbound.submitSend(
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            metadata);
    }
}

final class ZLinkSpotRoutedRequestCall implements ZLinkRequestCall {
    private final ZLinkSpotRoutedOutbound outbound;
    private final String routerChannelId;
    private final RoutingId targetNodeRid;
    private final String spotId;
    private final long spotGeneration;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final Duration timeout;
    private final ZLinkApplicationMetadata metadata;

    ZLinkSpotRoutedRequestCall(
        ZLinkSpotRoutedOutbound outbound,
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        this(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            null,
            timeout,
            ZLinkApplicationMetadata.empty());
    }

    ZLinkSpotRoutedRequestCall(
        ZLinkSpotRoutedOutbound outbound,
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Duration timeout) {
        this(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout,
            ZLinkApplicationMetadata.empty());
    }

    private ZLinkSpotRoutedRequestCall(
        ZLinkSpotRoutedOutbound outbound,
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Duration timeout,
        ZLinkApplicationMetadata metadata) {
        this.outbound = outbound;
        this.routerChannelId = routerChannelId;
        this.targetNodeRid = targetNodeRid;
        this.spotId = spotId;
        this.spotGeneration = spotGeneration;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.timeout = timeout;
        this.metadata = metadata;
    }

    public ZLinkRequestCall packetName(String packetName) {
        return new ZLinkSpotRoutedRequestCall(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            Optional.of(packetName),
            contentType,
            timeout,
            metadata);
    }

    @Override
    public ZLinkRequestCall metadata(String key, String value) {
        return new ZLinkSpotRoutedRequestCall(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout,
            metadata.with(key, value));
    }

    @Override
    public ZLinkRequestCall metadata(Map<String, String> values) {
        return new ZLinkSpotRoutedRequestCall(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout,
            metadata.withAll(values));
    }

    @Override
    public ZLinkRequestCall timeout(Duration timeout) {
        return new ZLinkSpotRoutedRequestCall(
            outbound,
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout,
            metadata);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectSameSpotWait(spotId);
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(
            outbound.submitRequest(
            routerChannelId,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            metadata,
            timeout,
            replyType));
    }

    @Override
    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.requireYieldAllowed("Spot request");
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
            .yieldCurrent(submit(replyType));
    }

}
