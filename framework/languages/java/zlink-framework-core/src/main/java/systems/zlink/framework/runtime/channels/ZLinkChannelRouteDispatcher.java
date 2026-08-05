package systems.zlink.framework.runtime.channels;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

final class ZLinkChannelRouteDispatcher {
    private final ZLinkChannelSocketRegistry sockets;
    private final ZLinkChannelDispatchRegistry registry;
    private final ZLinkSpotRouteBridgeRawReplies rawReplies;
    private final ZLinkChannelHandlerInvoker invoker;
    private final ZLinkChannelDispatchReporter errors;
    private final ZLinkMessageFlowTracer flow;
    private final ZLinkSpotRouteBridgeDrainer bridgeDrainer;
    private final Function<String, ZLinkBackendSpotRouteBridge> bridgeResolver;
    private final ZLinkInboundDispatchBudget inboundDispatchBudget;
    ZLinkChannelRouteDispatcher(
        ZLinkChannelSocketRegistry sockets,
        ZLinkChannelDispatchRegistry registry,
        ZLinkSpotRouteBridgeRawReplies rawReplies,
        ZLinkChannelHandlerInvoker invoker,
        ZLinkChannelDispatchReporter errors,
        ZLinkMessageFlowTracer flow,
        ZLinkSpotRouteBridgeDrainer bridgeDrainer,
        Function<String, ZLinkBackendSpotRouteBridge> bridgeResolver) {
        this(
            sockets,
            registry,
            rawReplies,
            invoker,
            errors,
            flow,
            bridgeDrainer,
            bridgeResolver,
            new ZLinkInboundDispatchBudget(0));
    }

    ZLinkChannelRouteDispatcher(
        ZLinkChannelSocketRegistry sockets,
        ZLinkChannelDispatchRegistry registry,
        ZLinkSpotRouteBridgeRawReplies rawReplies,
        ZLinkChannelHandlerInvoker invoker,
        ZLinkChannelDispatchReporter errors,
        ZLinkMessageFlowTracer flow,
        ZLinkSpotRouteBridgeDrainer bridgeDrainer,
        Function<String, ZLinkBackendSpotRouteBridge> bridgeResolver,
        ZLinkInboundDispatchBudget inboundDispatchBudget) {
        this.sockets = sockets;
        this.registry = registry;
        this.rawReplies = rawReplies;
        this.invoker = invoker;
        this.errors = errors;
        this.flow = flow;
        this.bridgeDrainer = bridgeDrainer;
        this.bridgeResolver = bridgeResolver;
        this.inboundDispatchBudget = inboundDispatchBudget;
    }

    void dispatch(
        String channelName,
        ZLinkBackendRouterSocket router,
        ZLinkBackendReceived received) {
        var incomingFlow = ZLinkChannelFlowFrame.decode(received.parts());
        var flowScope = incomingFlow == null ? null
            : systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.enter(incomingFlow);
        try {
            if (isProbeFrame(received.parts())) {
                return;
            }
            if (rawReplies.tryComplete(channelName, received)) {
                return;
            }
            if (dispatchBridgePacket(channelName, received)) {
                return;
            }
            ParsedPacket packet = parsePacket(received.parts());
            if (ZLinkFrameworkErrorReply.isPacketName(packet.packetName())) {
                errors.report(
                    ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                    received.requestSeq().isPresent()
                        ? ZLinkDispatchMessageKind.REQUEST
                        : ZLinkDispatchMessageKind.SEND,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    ZLinkDispatchErrorAction.DROP,
                    packet.packetName(),
                    channelName,
                    received.routingId().map(RoutingId::toString).orElse(null),
                    null);
                return;
            }
            if (received.routingId().isEmpty()) {
                errors.report(
                    ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                    ZLinkDispatchMessageKind.REQUEST,
                    ZLinkDispatchErrorReason.REPLY_PATH_MISSING,
                    ZLinkDispatchErrorAction.DROP,
                    packet.packetName(),
                    channelName,
                    null,
                    null);
                return;
            }
            String contentType = ZLinkChannelContentTypeFrame.decode(received.parts());
            RoutingId source = received.routingId().get();
            if (received.requestSeq().isEmpty()) {
                dispatchSend(channelName, source, packet, contentType);
                return;
            }
            long requestSeq = received.requestSeq().get();
            if (dispatchInternalRequest(channelName, router, source, requestSeq, packet)) {
                return;
            }
            ChannelRouteRequestHandlerRegistration registration =
                registry.routeRequestHandler(channelName, packet.packetName());
            if (registration == null && rawReplies.hasPending(channelName, source)) {
                return;
            }
            if (registration == null && rawReplies.discardRecentlyCompleted(channelName, received)) {
                return;
            }
            if (registration == null) {
                errors.replyError(
                    router,
                    source,
                    requestSeq,
                    ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                    ZLinkDispatchMessageKind.REQUEST,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    packet.packetName(),
                    channelName,
                    source.toString(),
                    null);
                return;
            }
            traceFlow(
                ZLinkMessageFlowOutcome.RECEIVED,
                ZLinkDispatchMessageKind.REQUEST,
                packet.packetName(),
                channelName,
                String.valueOf(requestSeq),
                source);
            dispatchRequestHandler(
                channelName,
                router,
                source,
                requestSeq,
                packet,
                registration,
                contentType);
        } finally {
            if (flowScope != null) flowScope.close();
            received.parts().forEach(Message::close);
        }
    }

    boolean dispatchBridgePacket(ZLinkBackendReceived received) {
        for (String channelName : sockets.spotRouteBridgeChannelNames()) {
            if (dispatchBridgePacket(channelName, received)) {
                return true;
            }
        }
        return false;
    }

    boolean dispatchBridgePacket(
        String channelName,
        ZLinkBackendReceived received) {
        if (received.routingId().isEmpty()
            || !ZLinkChannelRuntime.looksLikeSpotRouteBridgePacket(received.parts())) {
            return false;
        }
        ZLinkBackendSpotRouteBridge bridge = sockets.spotRouteBridge(channelName);
        if (bridge == null) {
            ChannelRegistration registration = sockets.registration(channelName);
            if (registration == null || registration.kind() != ChannelKind.ROUTE_MESH) {
                return false;
            }
            bridge = bridgeResolver.apply(channelName);
            if (bridge == null) {
                return false;
            }
        }
        RoutingId source = received.routingId().get();
        List<Message> parts = ZLinkChannelRuntime.copyMessages(received.parts());
        try {
            synchronized (bridge) {
                if (!bridge.handleRouterReceived(
                    channelName,
                    source,
                    received.requestSeq().orElse(0L),
                    parts)) {
                    return true;
                }
            }
            bridgeDrainer.drainNow(channelName);
        } catch (RuntimeException error) {
            errors.report(
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                received.requestSeq().isPresent()
                    ? ZLinkDispatchMessageKind.REQUEST
                    : ZLinkDispatchMessageKind.SEND,
                ZLinkDispatchErrorReason.INVALID_FRAME,
                ZLinkDispatchErrorAction.DROP,
                null,
                channelName,
                source.toString(),
                error);
        } finally {
            parts.forEach(Message::close);
        }
        return true;
    }

    private boolean dispatchInternalRequest(
        String channelName,
        ZLinkBackendRouterSocket router,
        RoutingId source,
        long requestSeq,
        ParsedPacket packet) {
        ZLinkChannelRuntime.RouteInternalRequestHandler handler =
            registry.internalRequest(packet.packetName());
        if (handler == null) {
            return false;
        }
        Message payload = Message.from(packet.payload());
        registry.routeRequestQueue(channelName).enqueueWithPayloadBytes(payload.size(), () ->
            handler.handle(source, payload)
                .thenAccept(reply -> ZLinkChannelDispatchReporter.replyAndClose(
                    router,
                    source,
                    requestSeq,
                    reply))
                .whenComplete((ignored, error) -> payload.close()));
        return true;
    }

    private void dispatchRequestHandler(
        String channelName,
        ZLinkBackendRouterSocket router,
        RoutingId source,
        long requestSeq,
        ParsedPacket packet,
        ChannelRouteRequestHandlerRegistration registration,
        String contentType) {
        Message payload = Message.from(packet.payload());
        ZLinkInboundDispatchBudget.Lease lease =
            inboundDispatchBudget.track(payload.size());
        try {
            CompletionStage<Void> queued = registry.routeRequestQueue(channelName)
                .enqueueWithPayloadBytes(payload.size(), () ->
                inboundDispatchBudget.acquireCompletionPermit()
                    .thenCompose(permit -> {
                        try {
                            return invokeStarted(lease, () -> invoker.executeHandler(() ->
                                invoker.invokeRouteRequestHandler(
                                    channelName,
                                    registration,
                                    source,
                                    payload,
                                    Map.of(),
                                    contentType)))
                                .whenComplete((reply, error) -> {
                                    try {
                                        if (error != null) {
                                            errors.replyError(
                                                router,
                                                source,
                                                requestSeq,
                                                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                                                ZLinkDispatchMessageKind.REQUEST,
                                                ZLinkChannelDispatchReporter.reasonFrom(error),
                                                packet.packetName(),
                                                channelName,
                                                source.toString(),
                                                error);
                                        } else {
                                            ZLinkChannelDispatchReporter.replyAndClose(
                                                router,
                                                source,
                                                requestSeq,
                                                reply);
                                            traceFlow(
                                                ZLinkMessageFlowOutcome.REPLIED,
                                                ZLinkDispatchMessageKind.REQUEST,
                                                packet.packetName(),
                                                channelName,
                                                String.valueOf(requestSeq),
                                                source);
                                        }
                                    } finally {
                                        permit.close();
                                    }
                                });
                        } catch (RuntimeException failure) {
                            permit.close();
                            return java.util.concurrent.CompletableFuture
                                .<Message>failedFuture(failure);
                        }
                    })
                    .whenComplete((ignored, error) -> {
                        payload.close();
                        lease.close();
                    })
                    .thenApply(ignored -> null));
            queued.whenComplete((ignored, error) -> {
                if (error != null) {
                    payload.close();
                    lease.close();
                }
            });
        } catch (RuntimeException error) {
            payload.close();
            lease.close();
            throw error;
        }
    }

    private void dispatchSend(
        String channelName,
        RoutingId source,
        ParsedPacket packet,
        String contentType) {
        ChannelRouteSendHandlerRegistration registration =
            registry.routeSendHandler(channelName, packet.packetName());
        if (registration == null) {
            errors.report(
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                ZLinkDispatchMessageKind.SEND,
                ZLinkDispatchErrorReason.HANDLER_MISSING,
                ZLinkDispatchErrorAction.DROP,
                packet.packetName(),
                channelName,
                null,
                null);
            return;
        }
        String packetName = packet.packetName();
        traceFlow(
            ZLinkMessageFlowOutcome.RECEIVED,
            ZLinkDispatchMessageKind.SEND,
            packetName,
            channelName,
            null,
            source);
        Message payload = Message.from(packet.payload());
        ZLinkInboundDispatchBudget.Lease lease =
            inboundDispatchBudget.track(payload.size());
        try {
            CompletionStage<Void> queued = registry.routeSendQueue(channelName)
                .enqueueWithPayloadBytes(payload.size(), () ->
                invokeStarted(lease, () -> invoker.executeHandler(() ->
                    invoker.invokeRouteSendHandler(
                        channelName, registration, source, payload, Map.of(), contentType)))
                    .whenComplete((ignored, error) -> {
                        if (error != null) {
                            errors.report(
                                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                                ZLinkDispatchMessageKind.SEND,
                                ZLinkChannelDispatchReporter.reasonFrom(error),
                                ZLinkDispatchErrorAction.DROP,
                                packetName,
                                channelName,
                                null,
                                error);
                        } else {
                            traceFlow(
                                ZLinkMessageFlowOutcome.DISPATCHED,
                                ZLinkDispatchMessageKind.SEND,
                                packetName,
                                channelName,
                                null,
                                source);
                        }
                    })
                    .whenComplete((ignored, error) -> {
                        payload.close();
                        lease.close();
                    }));
            queued.whenComplete((ignored, error) -> {
                if (error != null) {
                    payload.close();
                    lease.close();
                }
            });
        } catch (RuntimeException error) {
            payload.close();
            lease.close();
            throw error;
        }
    }

    private static <T> CompletionStage<T> invokeStarted(
        ZLinkInboundDispatchBudget.Lease lease,
        java.util.function.Supplier<CompletionStage<T>> invocation) {
        lease.handlerStarted();
        try {
            return invocation.get();
        } catch (RuntimeException error) {
            lease.close();
            throw error;
        }
    }

    private void traceFlow(
        ZLinkMessageFlowOutcome outcome,
        ZLinkDispatchMessageKind kind,
        String packetName,
        String channelName,
        String correlationId,
        RoutingId source) {
        if (flow.enabled(outcome)) {
            flow.trace(new ZLinkMessageFlowEvent(
                outcome,
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                kind,
                packetName,
                channelName,
                null,
                correlationId,
                source == null ? null : source.toString(),
                null,
                null,
                null));
        }
    }

    private static ParsedPacket parsePacket(List<Message> parts) {
        return parts.size() >= 2
            ? new ParsedPacket(parts.get(0).toUtf8String(), parts.get(1))
            : new ParsedPacket("", parts.get(0));
    }

    private static boolean isProbeFrame(List<Message> parts) {
        return parts.isEmpty() || parts.get(0).size() == 0;
    }
}
