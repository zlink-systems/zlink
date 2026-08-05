package systems.zlink.framework.runtime.channels;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

final class ZLinkChannelMessageDispatcher {
    private final ZLinkChannelDispatchRegistry registry;
    private final ZLinkChannelHandlerInvoker invoker;
    private final ZLinkChannelDispatchReporter errors;
    private final ZLinkMessageFlowTracer flow;
    private final ZLinkInboundDispatchBudget inboundDispatchBudget;
    ZLinkChannelMessageDispatcher(
        ZLinkChannelDispatchRegistry registry,
        ZLinkChannelHandlerInvoker invoker,
        ZLinkChannelDispatchReporter errors,
        ZLinkMessageFlowTracer flow) {
        this(registry, invoker, errors, flow, new ZLinkInboundDispatchBudget(0));
    }

    ZLinkChannelMessageDispatcher(
        ZLinkChannelDispatchRegistry registry,
        ZLinkChannelHandlerInvoker invoker,
        ZLinkChannelDispatchReporter errors,
        ZLinkMessageFlowTracer flow,
        ZLinkInboundDispatchBudget inboundDispatchBudget) {
        this.registry = registry;
        this.invoker = invoker;
        this.errors = errors;
        this.flow = flow;
        this.inboundDispatchBudget = inboundDispatchBudget;
    }

    void dispatchRequest(
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
            ParsedPacket packet = parsePacket(received.parts());
            if (ZLinkFrameworkErrorReply.isPacketName(packet.packetName())) {
                errors.report(
                    ZLinkDispatchErrorSurface.CHANNEL,
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
            RoutingId routingId = received.routingId().get();
            if (received.requestSeq().isEmpty()) {
                dispatchSend(channelName, packet, contentType);
                return;
            }
            ChannelRequestHandlerRegistration registration =
                registry.requestHandler(channelName, packet.packetName());
            long requestSeq = received.requestSeq().get();
            if (registration == null) {
                errors.replyError(
                    router,
                    routingId,
                    requestSeq,
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
                routingId,
                requestSeq,
                packet,
                registration,
                contentType);
        } finally {
            if (flowScope != null) flowScope.close();
            received.parts().forEach(Message::close);
        }
    }

    void dispatchPublish(String channelName, ZLinkBackendTopicMessage received) {
        var incomingFlow = ZLinkChannelFlowFrame.decode(received.parts());
        var flowScope = incomingFlow == null ? null
            : systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.enter(incomingFlow);
        try {
            String contentType = ZLinkChannelContentTypeFrame.decode(received.parts());
            ParsedPacket packet = parsePacket(received.parts());
            ChannelPublishHandlerRegistration registration =
                registry.publishHandler(channelName, packet.packetName());
            if (registration == null) {
                errors.report(
                    ZLinkDispatchErrorSurface.CHANNEL,
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
            traceFlow(
                ZLinkMessageFlowOutcome.RECEIVED,
                ZLinkDispatchMessageKind.PUBLISH,
                packetName,
                channelName,
                topic,
                null);
            Message payload = Message.from(packet.payload());
            ZLinkInboundDispatchBudget.Lease lease =
                inboundDispatchBudget.track(payload.size());
            try {
                CompletionStage<Void> queued = registry.publishQueue(channelName)
                    .enqueueWithPayloadBytes(payload.size(), () ->
                    invokeStarted(lease, () -> invoker.executeHandler(() ->
                        invoker.invokePublishHandler(
                            channelName, registration, topic, payload, contentType)))
                        .whenComplete((ignored, error) -> {
                            if (error != null) {
                                errors.report(
                                    ZLinkDispatchErrorSurface.CHANNEL,
                                    ZLinkDispatchMessageKind.PUBLISH,
                                    ZLinkChannelDispatchReporter.reasonFrom(error),
                                    ZLinkDispatchErrorAction.DROP,
                                    packetName,
                                    channelName,
                                    topic,
                                    null,
                                    error);
                            } else {
                                traceFlow(
                                    ZLinkMessageFlowOutcome.DISPATCHED,
                                    ZLinkDispatchMessageKind.PUBLISH,
                                    packetName,
                                    channelName,
                                    topic,
                                    null);
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
        } finally {
            if (flowScope != null) flowScope.close();
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
            null);
        Message payload = Message.from(packet.payload());
        ZLinkInboundDispatchBudget.Lease lease =
            inboundDispatchBudget.track(payload.size());
        try {
            CompletionStage<Void> queued = registry.sendQueue(channelName)
                .enqueueWithPayloadBytes(payload.size(), () ->
                invokeStarted(lease, () -> invoker.executeHandler(() ->
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
                                ZLinkMessageFlowOutcome.DISPATCHED,
                                ZLinkDispatchMessageKind.SEND,
                                packetName,
                                channelName,
                                null,
                                null);
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

    private void dispatchRequestHandler(
        String channelName,
        ZLinkBackendRouterSocket router,
        RoutingId routingId,
        long requestSeq,
        ParsedPacket packet,
        ChannelRequestHandlerRegistration registration,
        String contentType) {
        String packetName = packet.packetName();
        Message payload = Message.from(packet.payload());
        ZLinkInboundDispatchBudget.Lease lease =
            inboundDispatchBudget.track(payload.size());
        try {
            CompletionStage<Void> queued = registry.requestQueue(channelName)
                .enqueueWithPayloadBytes(payload.size(), () ->
                inboundDispatchBudget.acquireCompletionPermit()
                    .thenCompose(permit -> {
                        try {
                            return invokeStarted(lease, () -> invoker.executeHandler(() ->
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
                                                routingId,
                                                requestSeq,
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
                                                routingId,
                                                requestSeq,
                                                reply);
                                            traceFlow(
                                                ZLinkMessageFlowOutcome.REPLIED,
                                                ZLinkDispatchMessageKind.REQUEST,
                                                packetName,
                                                channelName,
                                                null,
                                                String.valueOf(requestSeq));
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
            String.valueOf(requestSeq));
    }

    private void traceFlow(
        ZLinkMessageFlowOutcome outcome,
        ZLinkDispatchMessageKind kind,
        String packetName,
        String channelName,
        String topic,
        String correlationId) {
        if (flow.enabled(outcome)) {
            flow.trace(new ZLinkMessageFlowEvent(
                outcome,
                ZLinkDispatchErrorSurface.CHANNEL,
                kind,
                packetName,
                channelName,
                topic,
                correlationId,
                null,
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
