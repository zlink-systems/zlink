package systems.zlink.framework.runtime.spots;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;


final class ZLinkSpotDirectOutbound {
    private final ZLinkSpotRouteMessages messages;
    private final Executor handlerExecutor;
    private final ZLinkMessageFlowTracer flow;
    ZLinkSpotDirectOutbound(
        ZLinkSpotRouteMessages messages,
        Executor handlerExecutor,
        ZLinkMessageFlowTracer flow) {
        this.messages = messages;
        this.handlerExecutor = handlerExecutor;
        this.flow = flow;
    }

    ZLinkSendCall send(
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName) {
        return send(
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            null);
    }

    ZLinkSendCall send(
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        return new ZLinkSpotDirectSendCall(
            this,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType);
    }

    ZLinkRequestCall request(
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        return request(
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            null,
            timeout);
    }

    ZLinkRequestCall request(
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Duration timeout) {
        return new ZLinkSpotDirectRequestCall(
            this,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout);
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
     * R1 value-passing (spec 27 §4): the ambient callback flow — or a new
     * APPLICATION flow for a first outbound started outside framework
     * callbacks — is captured as a value at submit time and passed to the
     * encoder explicitly. No scope is installed and no completion hop is added
     * to the returned stage, so the spot dispatch lane's turn stage stays the
     * bare admission future teardown relies on. At Off nothing is captured or
     * allocated.
     */
    private ZLinkFlowContext.State captureOutboundFlow() {
        if (!flow.captureEnabled()) {
            return null;
        }
        ZLinkFlowContext.State current = ZLinkFlowContext.current();
        return current != null
            ? current
            : ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
    }

    CompletionStage<Void> submitSend(
        ZLinkBackendSpot spot,
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
            null,
            targetNodeRid,
            spotId);
        List<Message> parts = messages.encodeSend(
            "", packetName, payload, contentType,
            metadata.values(), captureOutboundFlow());
        return ZLinkOneWayCalls.adaptOneWay(
            spot.sendToSpot(
                targetNodeRid,
                spotId,
                spotGeneration,
                metadata.encode(),
                parts))
            .whenComplete((ignored, failure) ->
                parts.forEach(Message::close))
            // Keep the admission future private. The public stage must not
            // allow callers to complete an operation that is still pending.
            .thenApply(ignored -> null);
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
        Class<TReply> replyType) {
        CompletableFuture<TReply> result = new CompletableFuture<>();
        List<Message> requestParts = messages.encodeRequest(
            "", packetName, payload, contentType,
            metadata.values(), captureOutboundFlow());
        AtomicBoolean requestPartsClosed = new AtomicBoolean();
        Runnable closeRequestParts = () -> {
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
            spotId);
        try {
            spot.requestToSpot(
                targetNodeRid,
                spotId,
                spotGeneration,
                metadata.encode(),
                requestParts,
                timeout)
                .whenComplete((reply, failure) -> {
                if (failure != null) {
                    closeRequestParts.run();
                    result.completeExceptionally(failure);
                    return;
                }
                trace(
                    ZLinkMessageFlowOutcome.REPLY_RECEIVED,
                    ZLinkDispatchMessageKind.RESPONSE,
                    packetName,
                    null,
                    targetNodeRid,
                    spotId);
                try {
                    if (reply.result() != ZLinkBackendRequestResult.OK) {
                        //  A backend request terminal is framework-generated;
                        //  carry the origin marker so a NotFound terminal
                        //  stays usable as the stale-route control signal.
                        result.completeExceptionally(
                            ZLinkFrameworkErrorOrigin.framework(
                                reply.result().toFrameworkErrorKind(reply.failureCode()),
                                "SPOT direct request failed: " + reply.result()));
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
        return result.thenApplyAsync(reply -> reply, handlerExecutor);
    }

    CompletionStage<Void> submitPublish(
        ZLinkBackendSpot spot,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        List<Message> parts = messages.encodePublish(
            channelName, topic, packetName, payload, contentType,
            metadata.values(), captureOutboundFlow());
        CompletionStage<Void> result = ZLinkOneWayCalls.adaptOneWay(
            spot.publishAsync(
                channelName,
                topic,
                metadata.encode(),
                parts,
                SendFlags.DONT_WAIT));
        result.whenComplete((ignored, failure) ->
            parts.forEach(Message::close));
        return result;
    }

    private void trace(
        ZLinkMessageFlowOutcome outcome,
        ZLinkDispatchMessageKind kind,
        Optional<String> packetName,
        String topic,
        RoutingId targetNodeRid,
        String spotId) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(outcome);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(new ZLinkMessageFlowEvent(
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
            null));
    }
}

final class ZLinkSpotDirectSendCall implements ZLinkSendCall {
    private final AtomicBoolean submitGate;
    private final ZLinkSpotDirectOutbound outbound;
    private final ZLinkBackendSpot spot;
    private final RoutingId targetNodeRid;
    private final String spotId;
    private final long spotGeneration;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    ZLinkSpotDirectSendCall(
        ZLinkSpotDirectOutbound outbound,
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName) {
        this(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            null,
            ZLinkApplicationMetadata.empty());
    }

    ZLinkSpotDirectSendCall(
        ZLinkSpotDirectOutbound outbound,
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        this(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            ZLinkApplicationMetadata.empty());
    }

    private ZLinkSpotDirectSendCall(
        ZLinkSpotDirectOutbound outbound,
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this(outbound, spot, targetNodeRid, spotId, spotGeneration, payload, packetName,
            contentType, metadata, new AtomicBoolean());
    }

    private ZLinkSpotDirectSendCall(
        ZLinkSpotDirectOutbound outbound,
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.outbound = outbound;
        this.spot = spot;
        this.targetNodeRid = targetNodeRid;
        this.spotId = spotId;
        this.spotGeneration = spotGeneration;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkSendCall packetName(String packetName) {
        return new ZLinkSpotDirectSendCall(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            Optional.of(packetName),
            contentType,
            metadata,
            submitGate);
    }

    @Override
    public ZLinkSendCall metadata(String key, String value) {
        return new ZLinkSpotDirectSendCall(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            metadata.with(key, value),
            submitGate);
    }

    @Override
    public ZLinkSendCall metadata(Map<String, String> values) {
        return new ZLinkSpotDirectSendCall(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            metadata.withAll(values),
            submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        return outbound.submitSend(
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            metadata);
    }
}

final class ZLinkSpotDirectRequestCall implements ZLinkRequestCall {
    private final AtomicBoolean submitGate;
    private final ZLinkSpotDirectOutbound outbound;
    private final ZLinkBackendSpot spot;
    private final RoutingId targetNodeRid;
    private final String spotId;
    private final long spotGeneration;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final Duration timeout;
    private final ZLinkApplicationMetadata metadata;

    ZLinkSpotDirectRequestCall(
        ZLinkSpotDirectOutbound outbound,
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        this(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            null,
            timeout,
            ZLinkApplicationMetadata.empty());
    }

    ZLinkSpotDirectRequestCall(
        ZLinkSpotDirectOutbound outbound,
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Duration timeout) {
        this(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout,
            ZLinkApplicationMetadata.empty());
    }

    private ZLinkSpotDirectRequestCall(
        ZLinkSpotDirectOutbound outbound,
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Duration timeout,
        ZLinkApplicationMetadata metadata) {
        this(outbound, spot, targetNodeRid, spotId, spotGeneration, payload, packetName,
            contentType, timeout, metadata, new AtomicBoolean());
    }

    private ZLinkSpotDirectRequestCall(
        ZLinkSpotDirectOutbound outbound,
        ZLinkBackendSpot spot,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Duration timeout,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.outbound = outbound;
        this.spot = spot;
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
        return new ZLinkSpotDirectRequestCall(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            Optional.of(packetName),
            contentType,
            timeout,
            metadata,
            submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(String key, String value) {
        return new ZLinkSpotDirectRequestCall(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout,
            metadata.with(key, value),
            submitGate);
    }

    @Override
    public ZLinkRequestCall metadata(Map<String, String> values) {
        return new ZLinkSpotDirectRequestCall(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout,
            metadata.withAll(values),
            submitGate);
    }

    @Override
    public ZLinkRequestCall timeout(Duration timeout) {
        return new ZLinkSpotDirectRequestCall(
            outbound,
            spot,
            targetNodeRid,
            spotId,
            spotGeneration,
            payload,
            packetName,
            contentType,
            timeout,
            metadata,
            submitGate);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        CompletionStage<TReply> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectSameSpotWait(spotId);
        return ZLinkAsyncSerialQueue.manageCurrent(
            outbound.submitRequest(
            spot,
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
        return ZLinkAsyncSerialQueue
            .yieldCurrent(submit(replyType));
    }

}

final class ZLinkSpotDirectPublishCall implements ZLinkPublishCall {
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
        this(outbound, spot, channelName, topic, payload, packetName, contentType, metadata,
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
            outbound, spot, channelName, topic, payload, Optional.of(packetName), contentType,
            metadata, submitGate);
    }

    @Override
    public ZLinkPublishCall metadata(String key, String value) {
        return new ZLinkSpotDirectPublishCall(
            outbound, spot, channelName, topic, payload, packetName, contentType,
            metadata.with(key, value), submitGate);
    }

    @Override
    public ZLinkPublishCall metadata(Map<String, String> values) {
        return new ZLinkSpotDirectPublishCall(
            outbound, spot, channelName, topic, payload, packetName, contentType,
            metadata.withAll(values), submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        return outbound.submitPublish(
            spot, channelName, topic, payload, packetName, contentType, metadata);
    }
}
