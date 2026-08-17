package systems.zlink.framework.runtime.channels;
import java.util.concurrent.CompletableFuture;
import java.util.function.Supplier;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;

final class ZLinkChannelRouteDispatcher {
    private final ZLinkChannelSocketRegistry sockets;
    private final ZLinkChannelDispatchRegistry registry;
    private final ZLinkChannelHandlerInvoker invoker;
    private final ZLinkChannelDispatchReporter errors;
    private final ZLinkMessageFlowTracer flow;
    private final ZLinkSpotRouteBridgeDrainer bridgeDrainer;
    private final Function<String, ZLinkBackendSpotRouteBridge> bridgeResolver;
    ZLinkChannelRouteDispatcher(
        ZLinkChannelSocketRegistry sockets,
        ZLinkChannelDispatchRegistry registry,
        ZLinkChannelHandlerInvoker invoker,
        ZLinkChannelDispatchReporter errors,
        ZLinkMessageFlowTracer flow,
        ZLinkSpotRouteBridgeDrainer bridgeDrainer,
        Function<String, ZLinkBackendSpotRouteBridge> bridgeResolver) {
        this.sockets = sockets;
        this.registry = registry;
        this.invoker = invoker;
        this.errors = errors;
        this.flow = flow;
        this.bridgeDrainer = bridgeDrainer;
        this.bridgeResolver = bridgeResolver;
    }

    void dispatch(
        String channelName,
        ZLinkBackendRouterSocket router,
        ZLinkBackendReceived received) {
        if (isProbeFrame(received.parts())) {
            received.parts().forEach(Message::close);
            return;
        }
        //  Spec 27 §4: decode and install the inbound flow pair (or start a new
        //  flow) only while capture is enabled; at Off suppress flow state.
        ZLinkFlowContext.State inboundFlow = null;
        boolean captureFlow = flow.captureEnabled();
        if (captureFlow) {
            try {
                inboundFlow = ZLinkChannelFlowFrame.decode(received.parts());
            } catch (PayloadDecodeDispatchException invalidFlow) {
                String packetName = received.parts().isEmpty()
                    ? null : received.parts().getFirst().toUtf8String();
                if (received.routingId().isPresent()
                    && received.requestSeq().isPresent()) {
                    errors.replyError(
                        router,
                        received.routingId().orElseThrow(),
                        received.requestSeq().orElseThrow(),
                        ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                        ZLinkDispatchMessageKind.REQUEST,
                        ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED,
                        packetName,
                        channelName,
                        received.routingId().orElseThrow().toString(),
                        invalidFlow);
                } else {
                    errors.report(
                        ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                        ZLinkDispatchMessageKind.SEND,
                        ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED,
                        ZLinkDispatchErrorAction.DROP,
                        packetName,
                        channelName,
                        null,
                        invalidFlow);
                }
                received.parts().forEach(Message::close);
                return;
            }
        }
        var flowScope = captureFlow
            ? ZLinkFlowContext.enterOrCreate(inboundFlow, ZLinkFlowOrigin.INBOUND)
            : ZLinkFlowContext.suppress();
        try {
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
                requestSeq,
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
            flowScope.close();
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
        registry.routeRequestQueue(channelName).enqueue(() ->
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
        try {
            CompletionStage<Void> queued = registry.routeRequestQueue(channelName)
                .enqueue(() ->
                CompletableFuture.completedFuture(null)
                    .thenCompose(permit -> {
                        try {
                            return invokeStarted(() -> invoker.executeHandler(() ->
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
                                                requestSeq,
                                                source);
                                        }
                                    } finally {

                                    }
                                });
                        } catch (RuntimeException failure) {

                            return CompletableFuture
                                .<Message>failedFuture(failure);
                        }
                    })
                    .whenComplete((ignored, error) -> {
                        payload.close();
                    })
                    .thenApply(ignored -> null));
            queued.whenComplete((ignored, error) -> {
                if (error != null) {
                    payload.close();
                }
            });
        } catch (RuntimeException error) {
            payload.close();
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
            0L,
            source);
        Message payload = Message.from(packet.payload());
        try {
            CompletionStage<Void> queued = registry.routeSendQueue(channelName)
                .enqueue(() ->
                invokeStarted(() -> invoker.executeHandler(() ->
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
                                0L,
                                source);
                        }
                    })
                    .whenComplete((ignored, error) -> {
                        payload.close();
                    }));
            queued.whenComplete((ignored, error) -> {
                if (error != null) {
                    payload.close();
                }
            });
        } catch (RuntimeException error) {
            payload.close();
            throw error;
        }
    }

    private static <T> CompletionStage<T> invokeStarted(
        Supplier<CompletionStage<T>> invocation) {
        return invocation.get();
    }

    /** Spec 26 §4: requestSeq 0 means none; strings are built after the gate. */
    private void traceFlow(
        ZLinkMessageFlowOutcome outcome,
        ZLinkDispatchMessageKind kind,
        String packetName,
        String channelName,
        long requestSeq,
        RoutingId source) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(outcome);
        if (tracePoint != null) {
            tracePoint.trace(new ZLinkMessageFlowEvent(
                outcome,
                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                kind,
                packetName,
                channelName,
                null,
                requestSeq == 0 ? null : String.valueOf(requestSeq),
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
