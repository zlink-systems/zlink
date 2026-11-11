package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkSpotDirectOutbound {
    private Runnable callAdmission = () -> {};

    void setCallAdmission(Runnable value) {
        callAdmission = value;
    }

    void admitCall() {
        callAdmission.run();
    }

    private final ZLinkSpotRouteMessages messages;
    private final ZLinkMessageFlowTracer flow;

    ZLinkSpotDirectOutbound(ZLinkSpotRouteMessages messages, ZLinkMessageFlowTracer flow) {
        this.messages = messages;
        this.flow = flow;
    }

    ZLinkPublishCall publish(
            ZLinkBackendSpot spot,
            String channelName,
            String topic,
            Message payload,
            Optional<String> packetName) {
        return publish(spot, channelName, topic, payload, packetName, null);
    }

    ZLinkPublishCall publish(
            ZLinkBackendSpot spot,
            String channelName,
            String topic,
            Message payload,
            Optional<String> packetName,
            String contentType) {
        return new ZLinkSpotDirectPublishCall(
                this, spot, channelName, topic, payload, packetName, contentType);
    }

    /**
     * R1 value-passing (spec 27 §4): the ambient callback flow — or a new APPLICATION flow for a
     * first outbound started outside framework callbacks — is captured as a value at submit time
     * and passed to the encoder explicitly and no flow scope is installed. The terminal layer
     * separately restores an active serial turn only when admission is pending. At Off nothing is
     * captured or allocated.
     */
    private ZLinkFlowContext.State captureOutboundFlow() {
        if (!flow.captureEnabled()) {
            return null;
        }
        ZLinkFlowContext.State current = ZLinkFlowContext.current();
        return current != null ? current : ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
    }

    CompletionStage<Void> submitSend(
            ZLinkBackendSpot spot,
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
                null,
                targetNodeRid,
                spotId);
        List<Message> parts =
                messages.encodeSend(
                        "",
                        packetName,
                        payload,
                        contentType,
                        metadata.values(),
                        captureOutboundFlow());
        CompletionStage<Void> submission;
        try {
            submission =
                    spot.sendToSpot(
                            targetNodeRid, spotId, spotGeneration, metadata.encode(), parts);
        } catch (RuntimeException | Error failure) {
            ZLinkFrameworkMultipartView.closeAll(parts);
            throw failure;
        }
        return ZLinkOneWayCalls.adaptOneWay(
                submission.whenComplete(
                        (ignored, failure) -> ZLinkFrameworkMultipartView.closeAll(parts)));
    }

    <TReply> CompletionStage<TReply> submitRequest(
            ZLinkBackendSpot spot,
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
        CompletableFuture<TReply> result = new CompletableFuture<>();
        var outboundFlow = captureOutboundFlow();
        result.whenComplete(
                (reply, failure) -> {
                    var terminal = flow.beginRequestTerminal(failure, result);
                    if (terminal == null) return;
                    Throwable cause = failure;
                    while ((cause instanceof java.util.concurrent.CompletionException
                                    || cause instanceof java.util.concurrent.ExecutionException)
                            && cause.getCause() != null) cause = cause.getCause();
                    terminal.trace(
                            new ZLinkMessageFlowEvent(
                                    ZLinkMessageFlowOutcome.REPLY_RECEIVED,
                                    ZLinkDispatchErrorSurface.SPOT_ROUTE,
                                    ZLinkDispatchMessageKind.REQUEST,
                                    packetName.orElse(null),
                                    null,
                                    null,
                                    null,
                                    targetNodeRid.toString(),
                                    spotId,
                                    null,
                                    null,
                                    null,
                                    null,
                                    cause == null ? null : cause.getClass().getSimpleName(),
                                    cause == null ? null : cause.getMessage(),
                                    outboundFlow == null ? null : outboundFlow.flowId(),
                                    outboundFlow == null ? null : outboundFlow.origin()));
                });
        List<Message> requestParts =
                messages.encodeRequest(
                        "", packetName, payload, contentType, metadata.values(), outboundFlow);
        AtomicBoolean requestPartsClosed = new AtomicBoolean();
        Runnable closeRequestParts =
                () -> {
                    if (requestPartsClosed.compareAndSet(false, true)) {
                        requestParts.forEach(Message::close);
                    }
                };
        trace(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchMessageKind.REQUEST,
                packetName,
                null,
                targetNodeRid,
                spotId,
                outboundFlow);
        try {
            spot.requestToSpot(
                            targetNodeRid,
                            spotId,
                            spotGeneration,
                            metadata.encode(),
                            requestParts,
                            timeout,
                            requestAdmission)
                    .whenComplete(
                            (reply, failure) -> {
                                if (failure != null) {
                                    closeRequestParts.run();
                                    result.completeExceptionally(failure);
                                    return;
                                }
                                try {
                                    if (reply.result() != ZLinkBackendRequestResult.OK) {
                                        //  A backend request terminal is framework-generated;
                                        //  carry the origin marker so a NotFound terminal
                                        //  stays usable as the stale-route control signal.
                                        result.completeExceptionally(
                                                ZLinkFrameworkErrorOrigin.framework(
                                                        reply.result()
                                                                .toFrameworkErrorKind(
                                                                        reply.failureCode()),
                                                        "SPOT direct request failed: "
                                                                + reply.result(),
                                                        reply.failureCode()));
                                        return;
                                    }
                                    result.complete(messages.decodeReply(reply.parts(), replyType));
                                } catch (RuntimeException ex) {
                                    result.completeExceptionally(ex);
                                } finally {
                                    reply.close();
                                    closeRequestParts.run();
                                }
                            });
        } catch (RuntimeException ex) {
            closeRequestParts.run();
            result.completeExceptionally(ex);
        }
        return result;
    }

    CompletionStage<Void> submitPublish(
            ZLinkBackendSpot spot,
            String channelName,
            String topic,
            Message payload,
            Optional<String> packetName,
            String contentType,
            ZLinkApplicationMetadata metadata) {
        List<Message> parts =
                messages.encodePublish(
                        channelName,
                        topic,
                        packetName,
                        payload,
                        contentType,
                        metadata.values(),
                        captureOutboundFlow());
        CompletionStage<Void> result =
                spot.publishAsync(
                        channelName, topic, metadata.encode(), parts, SendFlags.DONT_WAIT);
        result.whenComplete((ignored, failure) -> parts.forEach(Message::close));
        return ZLinkOneWayCalls.adaptOneWay(result);
    }

    private void trace(
            ZLinkMessageFlowOutcome outcome,
            ZLinkDispatchMessageKind kind,
            Optional<String> packetName,
            String topic,
            RoutingId targetNodeRid,
            String spotId) {
        trace(outcome, kind, packetName, topic, targetNodeRid, spotId, null);
    }

    private void trace(
            ZLinkMessageFlowOutcome outcome,
            ZLinkDispatchMessageKind kind,
            Optional<String> packetName,
            String topic,
            RoutingId targetNodeRid,
            String spotId,
            ZLinkFlowContext.State context) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(outcome);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(
                new ZLinkMessageFlowEvent(
                                outcome,
                                topic == null
                                        ? ZLinkDispatchErrorSurface.SPOT_ROUTE
                                        : ZLinkDispatchErrorSurface.SPOT_SUBSCRIPTION,
                                kind,
                                packetName.orElse(null),
                                null,
                                topic,
                                null,
                                targetNodeRid == null ? null : targetNodeRid.toString(),
                                spotId == null ? null : spotId.toString(),
                                null,
                                null)
                        .withFlow(
                                context == null ? null : context.flowId(),
                                context == null ? null : context.origin()));
    }
}

final class ZLinkSpotDirectPublishCall implements ZLinkPublishCall, Runnable {
    @Override
    public void run() {
        outbound.admitCall();
    }

    private final AtomicBoolean submitGate;
    private final ZLinkSpotDirectOutbound outbound;
    private final ZLinkBackendSpot spot;
    private final String channelName;
    private final String topic;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    ZLinkSpotDirectPublishCall(
            ZLinkSpotDirectOutbound outbound,
            ZLinkBackendSpot spot,
            String channelName,
            String topic,
            Message payload,
            Optional<String> packetName) {
        this(
                outbound,
                spot,
                channelName,
                topic,
                payload,
                packetName,
                null,
                ZLinkApplicationMetadata.empty());
    }

    ZLinkSpotDirectPublishCall(
            ZLinkSpotDirectOutbound outbound,
            ZLinkBackendSpot spot,
            String channelName,
            String topic,
            Message payload,
            Optional<String> packetName,
            String contentType) {
        this(
                outbound,
                spot,
                channelName,
                topic,
                payload,
                packetName,
                contentType,
                ZLinkApplicationMetadata.empty());
    }

    private ZLinkSpotDirectPublishCall(
            ZLinkSpotDirectOutbound outbound,
            ZLinkBackendSpot spot,
            String channelName,
            String topic,
            Message payload,
            Optional<String> packetName,
            String contentType,
            ZLinkApplicationMetadata metadata) {
        this(
                outbound,
                spot,
                channelName,
                topic,
                payload,
                packetName,
                contentType,
                metadata,
                new AtomicBoolean());
    }

    private ZLinkSpotDirectPublishCall(
            ZLinkSpotDirectOutbound outbound,
            ZLinkBackendSpot spot,
            String channelName,
            String topic,
            Message payload,
            Optional<String> packetName,
            String contentType,
            ZLinkApplicationMetadata metadata,
            AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.outbound = outbound;
        this.spot = spot;
        this.channelName = channelName;
        this.topic = topic;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkPublishCall packetName(String packetName) {
        return new ZLinkSpotDirectPublishCall(
                outbound,
                spot,
                channelName,
                topic,
                payload,
                Optional.of(packetName),
                contentType,
                metadata,
                submitGate);
    }

    @Override
    public ZLinkPublishCall metadata(String key, String value) {
        return new ZLinkSpotDirectPublishCall(
                outbound,
                spot,
                channelName,
                topic,
                payload,
                packetName,
                contentType,
                metadata.with(key, value),
                submitGate);
    }

    @Override
    public ZLinkPublishCall metadata(Map<String, String> values) {
        return new ZLinkSpotDirectPublishCall(
                outbound,
                spot,
                channelName,
                topic,
                payload,
                packetName,
                contentType,
                metadata.withAll(values),
                submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate, this);
        if (duplicate != null) {
            return duplicate;
        }
        CompletionStage<Void> result =
                outbound.submitPublish(
                        spot, channelName, topic, payload, packetName, contentType, metadata);
        return result.toCompletableFuture().isDone()
                ? result
                : ZLinkSerialExecutionQueue.manageCurrent(result);
    }
}
