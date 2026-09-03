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
            received.close();
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
                if (received.routingId().isPresent() && received.isRequest()) {
                    errors.replyError(
                        router,
                        received,
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
                if (received.isRequest()) {
                    received.closeParts();
                } else {
                    received.close();
                }
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
            ParsedPacket packet;
            try {
                packet = parsePacket(received.parts());
            } catch (systems.zlink.framework.errors.ZLinkFrameworkException invalidEnvelope) {
                //  A JSON-object first frame that is not a valid shared
                //  envelope is a protocol error (C++ decode parity).
                if (received.routingId().isPresent() && received.isRequest()) {
                    errors.replyError(
                        router,
                        received,
                        ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                        ZLinkDispatchMessageKind.REQUEST,
                        ZLinkDispatchErrorReason.INVALID_FRAME,
                        null,
                        channelName,
                        received.routingId().orElseThrow().toString(),
                        invalidEnvelope);
                } else {
                    errors.report(
                        ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                        ZLinkDispatchMessageKind.SEND,
                        ZLinkDispatchErrorReason.INVALID_FRAME,
                        ZLinkDispatchErrorAction.DROP,
                        null,
                        channelName,
                        null,
                        invalidEnvelope);
                }
                return;
            }
            if (ZLinkFrameworkErrorReply.isPacketName(packet.packetName())
                || (packet.header() != null && packet.header().isError())) {
                errors.report(
                    ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                    received.isRequest()
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
            String contentType = packet.header() != null
                ? packet.header().contentType()
                : ZLinkChannelContentTypeFrame.decode(received.parts());
            RoutingId source = received.routingId().get();
            if (!received.isRequest()) {
                dispatchSend(channelName, source, packet, contentType);
                return;
            }
            long requestSeq = received.requestSeq().orElse(0L);
            if (dispatchInternalRequest(
                    channelName, router, received, source, packet)) {
                return;
            }
            ChannelRouteRequestHandlerRegistration registration =
                registry.routeRequestHandler(channelName, packet.packetName());
            if (registration == null) {
                errors.replyError(
                    router,
                    received,
                    ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                    ZLinkDispatchMessageKind.REQUEST,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    packet.packetName(),
                    channelName,
                    source.toString(),
                    packet.header(),
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
                received,
                source,
                requestSeq,
                packet,
                registration,
                contentType);
        } finally {
            flowScope.close();
            if (received.isRequest()) {
                received.closeParts();
            } else {
                received.close();
            }
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
                received.isRequest()
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
        ZLinkBackendReceived received,
        RoutingId source,
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
                    received,
                    reply))
                .whenComplete((ignored, error) -> payload.close()));
        return true;
    }

    private void dispatchRequestHandler(
        String channelName,
        ZLinkBackendRouterSocket router,
        ZLinkBackendReceived received,
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
                                    packet.header() != null
                                        ? packet.header().metadata()
                                        : Map.of(),
                                    contentType)))
                                .whenComplete((reply, error) -> {
                                    try {
                                        if (error != null) {
                                            errors.replyError(
                                                router,
                                                received,
                                                ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                                                ZLinkDispatchMessageKind.REQUEST,
                                                ZLinkChannelDispatchReporter.reasonFrom(error),
                                                packet.packetName(),
                                                channelName,
                                                source.toString(),
                                                packet.header(),
                                                error);
                                        } else {
                                            ZLinkChannelDispatchReporter.replyPayloadAndClose(
                                                router,
                                                received,
                                                packet.header(),
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
                        channelName, registration, source, payload,
                        packet.header() != null ? packet.header().metadata() : Map.of(),
                        contentType)))
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

    /**
     * Parses an inbound route mesh message. A shared cross-language envelope
     * yields the header's messageName and body; legacy raw parts keep the
     * packet-name/payload frames. A JSON-object first frame that is not a
     * valid envelope throws {@code PROTOCOL_ERROR}.
     */
    private static ParsedPacket parsePacket(List<Message> parts) {
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header header = systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeDispatchHeader(parts, false);
        if (header != null) {
            return new ParsedPacket(header.messageName(), parts.get(1), header);
        }
        return parts.size() >= 2
            ? new ParsedPacket(parts.get(0).toUtf8String(), parts.get(1))
            : new ParsedPacket("", parts.get(0));
    }

    private static boolean isProbeFrame(List<Message> parts) {
        return parts.isEmpty() || parts.get(0).size() == 0;
    }
}
