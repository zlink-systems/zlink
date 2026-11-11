package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.logging.Level;
import java.util.logging.Logger;

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

    /**
     * R1 value-passing (spec 27 §4): capture the ambient callback flow — or start an APPLICATION
     * flow for a first outbound outside framework callbacks — as a value for the encoder. No scope
     * install, no wrapper on the returned stage; at Off nothing is captured or allocated.
     */
    private ZLinkFlowContext.State captureOutboundFlow() {
        if (!flow.captureEnabled()) {
            return null;
        }
        ZLinkFlowContext.State current = ZLinkFlowContext.current();
        return current != null ? current : ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
    }

    CompletionStage<Void> submitSend(
            String routerChannelId,
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            ZLinkFrameworkMultipartView payload,
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
            return directOutbound.submitSend(
                    routerNode.entrySpot(),
                    targetNodeRid,
                    spotId,
                    spotGeneration,
                    payload,
                    packetName,
                    contentType,
                    metadata);
        }
        if (!metadata.values().isEmpty()) {
            throw new UnsupportedOperationException(
                    "legacy routed Spot transport does not support application metadata");
        }
        List<Message> parts =
                messages.encodeSend(
                        routerChannelId,
                        packetName,
                        payload,
                        contentType,
                        metadata.values(),
                        captureOutboundFlow());
        try {
            return ZLinkOneWayCalls.adaptOneWay(
                    channels.sendToSpotViaRouterChannel(
                                    routerChannelId, targetNodeRid, spotId, spotGeneration, parts)
                            .whenComplete(
                                    (ignored, error) -> {
                                        ZLinkFrameworkMultipartView.closeAll(parts);
                                        if (error != null) {
                                            Logger.getLogger(
                                                            ZLinkSpotRoutedOutbound.class.getName())
                                                    .log(
                                                            Level.SEVERE,
                                                            "one-way routed SPOT submission failed",
                                                            error);
                                        }
                                    }));
        } catch (RuntimeException error) {
            ZLinkFrameworkMultipartView.closeAll(parts);
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
            Class<TReply> replyType,
            CompletableFuture<Void> requestAdmission) {
        trace(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchMessageKind.REQUEST,
                packetName,
                routerChannelId,
                spotId);
        ZLinkInternalSpotNode routerNode = localRouterNode.apply(routerChannelId);
        if (routerNode != null) {
            return directOutbound.submitRequest(
                    routerNode.entrySpot(),
                    targetNodeRid,
                    spotId,
                    spotGeneration,
                    payload,
                    packetName,
                    contentType,
                    metadata,
                    timeout,
                    replyType,
                    requestAdmission);
        }
        if (!metadata.values().isEmpty()) {
            throw new UnsupportedOperationException(
                    "legacy routed Spot transport does not support application metadata");
        }
        List<Message> parts =
                messages.encodeRequest(
                        routerChannelId,
                        packetName,
                        payload,
                        contentType,
                        metadata.values(),
                        captureOutboundFlow());
        try {
            return channels.requestToSpotViaRouterChannel(
                            routerChannelId,
                            targetNodeRid,
                            spotId,
                            spotGeneration,
                            parts,
                            timeout,
                            requestAdmission)
                    .thenApply(
                            replyParts -> {
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
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(outcome);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(
                new ZLinkMessageFlowEvent(
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
