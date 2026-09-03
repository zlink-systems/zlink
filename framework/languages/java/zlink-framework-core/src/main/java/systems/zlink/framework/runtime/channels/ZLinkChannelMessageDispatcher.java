package systems.zlink.framework.runtime.channels;
import java.util.concurrent.CompletableFuture;
import java.util.function.Supplier;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;

final class ZLinkChannelMessageDispatcher {
    private final ZLinkChannelDispatchRegistry registry;
    private final ZLinkChannelHandlerInvoker invoker;
    private final ZLinkChannelDispatchReporter errors;
    private final ZLinkMessageFlowTracer flow;
    ZLinkChannelMessageDispatcher(
        ZLinkChannelDispatchRegistry registry,
        ZLinkChannelHandlerInvoker invoker,
        ZLinkChannelDispatchReporter errors,
        ZLinkMessageFlowTracer flow) {
        this.registry = registry;
        this.invoker = invoker;
        this.errors = errors;
        this.flow = flow;
    }

    void dispatchRequest(
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
                        ZLinkDispatchErrorSurface.CHANNEL,
                        ZLinkDispatchMessageKind.REQUEST,
                        ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED,
                        packetName,
                        channelName,
                        received.routingId().orElseThrow().toString(),
                        invalidFlow);
                } else {
                    errors.report(
                        ZLinkDispatchErrorSurface.CHANNEL,
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
            ParsedPacket packet = parsePacket(received.parts());
            if (ZLinkFrameworkErrorReply.isPacketName(packet.packetName())) {
                errors.report(
                    ZLinkDispatchErrorSurface.CLASSIC_FANOUT,
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
                    ZLinkDispatchErrorSurface.CHANNEL,
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
            if (!received.isRequest()) {
                dispatchSend(channelName, packet, contentType);
                return;
            }
            ChannelRequestHandlerRegistration registration =
                registry.requestHandler(channelName, packet.packetName());
            long requestSeq = received.requestSeq().orElse(0L);
            if (registration == null) {
                errors.replyError(
                    router,
                    received,
                    ZLinkDispatchErrorSurface.CHANNEL,
                    ZLinkDispatchMessageKind.REQUEST,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    packet.packetName(),
                    channelName,
                    null,
                    null);
                return;
            }
            traceReceivedRequest(channelName, packet.packetName(), requestSeq);
            dispatchRequestHandler(
                channelName,
                router,
                received,
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

    void dispatchPublish(String channelName, ZLinkBackendTopicMessage received) {
        ZLinkFlowContext.State inboundFlow = null;
        boolean captureFlow = flow.captureEnabled();
        if (captureFlow) {
            try {
                inboundFlow = ZLinkChannelFlowFrame.decode(received.parts());
            } catch (PayloadDecodeDispatchException invalidFlow) {
                errors.report(
                    ZLinkDispatchErrorSurface.CLASSIC_FANOUT,
                    ZLinkDispatchMessageKind.PUBLISH,
                    ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED,
                    ZLinkDispatchErrorAction.DROP,
                    received.parts().isEmpty()
                        ? null : received.parts().getFirst().toUtf8String(),
                    channelName,
                    received.topic(),
                    null,
                    invalidFlow);
                received.parts().forEach(Message::close);
                return;
            }
        }
        var flowScope = captureFlow
            ? ZLinkFlowContext.enterOrCreate(inboundFlow, ZLinkFlowOrigin.INBOUND)
            : ZLinkFlowContext.suppress();
        try {
            String contentType = ZLinkChannelContentTypeFrame.decode(received.parts());
            ParsedPacket packet = parsePacket(received.parts());
            ChannelPublishHandlerRegistration registration =
                registry.publishHandler(channelName, packet.packetName());
            if (registration == null) {
                errors.report(
                    ZLinkDispatchErrorSurface.CLASSIC_FANOUT,
                    ZLinkDispatchMessageKind.PUBLISH,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    ZLinkDispatchErrorAction.DROP,
                    packet.packetName(),
                    channelName,
                    received.topic(),
                    null,
                    null);
                return;
            }
            String packetName = packet.packetName();
            String topic = received.topic();
            Message payload = Message.from(packet.payload());
            try {
                CompletionStage<Void> queued = registry.publishQueue(channelName)
                    .enqueue(() ->
                    invokeStarted(() -> invoker.executeHandler(() ->
                        invoker.invokePublishHandler(
                            channelName, registration, topic, payload, contentType)))
                        .whenComplete((ignored, error) -> {
                            if (error != null) {
                                errors.report(
                                    ZLinkDispatchErrorSurface.CLASSIC_FANOUT,
                                    ZLinkDispatchMessageKind.PUBLISH,
                                    ZLinkChannelDispatchReporter.reasonFrom(error),
                                    ZLinkDispatchErrorAction.DROP,
                                    packetName,
                                    channelName,
                                    topic,
                                    null,
                                    error);
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
        } finally {
            flowScope.close();
            received.parts().forEach(Message::close);
        }
    }

    private void dispatchSend(
        String channelName,
        ParsedPacket packet,
        String contentType) {
        ChannelSendHandlerRegistration registration =
            registry.sendHandler(channelName, packet.packetName());
        if (registration == null) {
            errors.report(
                ZLinkDispatchErrorSurface.CHANNEL,
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
            0L);
        Message payload = Message.from(packet.payload());
        try {
            CompletionStage<Void> queued = registry.sendQueue(channelName)
                .enqueue(() -> {
                    traceFlow(
                        ZLinkMessageFlowOutcome.ADMITTED,
                        ZLinkDispatchMessageKind.SEND,
                        packetName,
                        channelName,
                        null,
                        0L);
                    traceFlow(
                        ZLinkMessageFlowOutcome.DISPATCHED,
                        ZLinkDispatchMessageKind.SEND,
                        packetName,
                        channelName,
                        null,
                        0L);
                    return invokeStarted(() -> invoker.executeHandler(() ->
                        invoker.invokeSendHandler(
                            channelName, registration, payload, Map.of(), contentType)))
                        .whenComplete((ignored, error) -> {
                            if (error != null) {
                                errors.report(
                                    ZLinkDispatchErrorSurface.CHANNEL,
                                    ZLinkDispatchMessageKind.SEND,
                                    ZLinkChannelDispatchReporter.reasonFrom(error),
                                    ZLinkDispatchErrorAction.DROP,
                                    packetName,
                                    channelName,
                                    null,
                                    error);
                            } else {
                                traceFlow(
                                    ZLinkMessageFlowOutcome.COMPLETED,
                                    ZLinkDispatchMessageKind.SEND,
                                    packetName,
                                    channelName,
                                    null,
                                    0L);
                            }
                        })
                        .whenComplete((ignored, error) -> payload.close());
                });
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

    private void dispatchRequestHandler(
        String channelName,
        ZLinkBackendRouterSocket router,
        ZLinkBackendReceived received,
        long requestSeq,
        ParsedPacket packet,
        ChannelRequestHandlerRegistration registration,
        String contentType) {
        String packetName = packet.packetName();
        Message payload = Message.from(packet.payload());
        try {
            CompletionStage<Void> queued = registry.requestQueue(channelName)
                .enqueue(() -> {
                traceFlow(
                    ZLinkMessageFlowOutcome.ADMITTED,
                    ZLinkDispatchMessageKind.REQUEST,
                    packetName,
                    channelName,
                    null,
                    requestSeq);
                traceFlow(
                    ZLinkMessageFlowOutcome.DISPATCHED,
                    ZLinkDispatchMessageKind.REQUEST,
                    packetName,
                    channelName,
                    null,
                    requestSeq);
                return CompletableFuture.completedFuture(null)
                    .thenCompose(permit -> {
                        try {
                            return invokeStarted(() -> invoker.executeHandler(() ->
                                invoker.invokeRequestHandler(
                                    channelName,
                                    registration,
                                    payload,
                                    Map.of(),
                                    contentType)))
                                .whenComplete((reply, error) -> {
                                    try {
                                        if (error != null) {
                                            errors.replyError(
                                                router,
                                                received,
                                                ZLinkDispatchErrorSurface.CHANNEL,
                                                ZLinkDispatchMessageKind.REQUEST,
                                                ZLinkChannelDispatchReporter.reasonFrom(error),
                                                packetName,
                                                channelName,
                                                null,
                                                error);
                                        } else {
                                            ZLinkChannelDispatchReporter.replyAndClose(
                                                router,
                                                received,
                                                reply);
                                            traceFlow(
                                                ZLinkMessageFlowOutcome.REPLIED,
                                                ZLinkDispatchMessageKind.REQUEST,
                                                packetName,
                                                channelName,
                                                null,
                                                requestSeq);
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
                    .thenApply(ignored -> null);
                });
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

    private void traceReceivedRequest(
        String channelName,
        String packetName,
        long requestSeq) {
        traceFlow(
            ZLinkMessageFlowOutcome.RECEIVED,
            ZLinkDispatchMessageKind.REQUEST,
            packetName,
            channelName,
            null,
            requestSeq);
    }

    /** Spec 26 §4: requestSeq 0 means none; strings are built after the gate. */
    private void traceFlow(
        ZLinkMessageFlowOutcome outcome,
        ZLinkDispatchMessageKind kind,
        String packetName,
        String channelName,
        String topic,
        long requestSeq) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(outcome);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(new ZLinkMessageFlowEvent(
            outcome,
            ZLinkDispatchErrorSurface.CHANNEL,
            kind,
            packetName,
            channelName,
            topic,
            requestSeq == 0 ? null : String.valueOf(requestSeq),
            null,
            null,
            null,
            null));
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
