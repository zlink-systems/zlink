package systems.zlink.framework.runtime.spots;

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
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;


final class ZLinkSpotDirectOutbound {
    private final ZLinkSpotRouteMessages messages;
    private final Executor handlerExecutor;
    private final ZLinkMessageFlowTracer flow;
    private final ZLinkOneWayCalls oneWayCalls;

    ZLinkSpotDirectOutbound(
        ZLinkSpotRouteMessages messages,
        Executor handlerExecutor,
        ZLinkMessageFlowTracer flow) {
        this(
            messages,
            handlerExecutor,
            flow,
            new ZLinkOneWayCalls((backend, key) -> (submission, cleanup) -> {
                try {
                    return submission.get()
                        ? java.util.concurrent.CompletableFuture.completedFuture(null)
                        : java.util.concurrent.CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "one-way submission was not admitted"));
                } finally {
                    cleanup.run();
                }
            }));
    }

    ZLinkSpotDirectOutbound(
        ZLinkSpotRouteMessages messages,
        Executor handlerExecutor,
        ZLinkMessageFlowTracer flow,
        ZLinkOneWayCalls oneWayCalls) {
        this.messages = messages;
        this.handlerExecutor = handlerExecutor;
        this.flow = flow;
        this.oneWayCalls = oneWayCalls;
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
        List<Message> parts = messages.encode(packetName, payload, contentType);
        return oneWayCalls.submitOneWay(
            spot,
            ZLinkBackendAdmissionKey.spot(targetNodeRid, spotId),
            () -> spot.sendToSpot(
                targetNodeRid,
                spotId,
                spotGeneration,
                metadata.encode(),
                parts,
                SendFlags.DONT_WAIT),
            () -> parts.forEach(Message::close));
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
        List<Message> requestParts = messages.encode(packetName, payload, contentType);
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
            boolean submitted = spot.requestToSpot(
                targetNodeRid,
                spotId,
                spotGeneration,
                metadata.encode(),
                requestParts,
                reply -> {
                trace(
                    ZLinkMessageFlowOutcome.REPLY_RECEIVED,
                    ZLinkDispatchMessageKind.RESPONSE,
                    packetName,
                    null,
                    targetNodeRid,
                    spotId);
                try {
                    if (reply.result() != ZLinkBackendRequestResult.OK) {
                        ZLinkFrameworkErrorKind errorKind = reply.result()
                            == ZLinkBackendRequestResult.NOT_FOUND
                            ? ZLinkFrameworkErrorKind.NOT_FOUND
                            : ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
                        result.completeExceptionally(new ZLinkFrameworkException(
                            errorKind,
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
            }, SendFlags.NONE, timeout);
            if (!submitted) {
                closeRequestParts.run();
                result.completeExceptionally(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                    "SPOT direct request was not admitted"));
            }
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
        trace(
            ZLinkMessageFlowOutcome.SENT,
            ZLinkDispatchMessageKind.PUBLISH,
            packetName,
            topic,
            null,
            null);
        List<Message> parts = messages.encode(packetName, payload, contentType);
        return oneWayCalls.submitOneWay(
                spot,
                ZLinkBackendAdmissionKey.channel(channelName),
                () -> {
                    spot.publish(
                        channelName,
                        topic,
                        metadata.encode(),
                        parts,
                        SendFlags.DONT_WAIT);
                    // Target-specific acceptance is outside the publish terminal.
                    return true;
                },
                () -> parts.forEach(Message::close))
            .thenApply(ignored -> null);
    }

    private void trace(
        ZLinkMessageFlowOutcome outcome,
        ZLinkDispatchMessageKind kind,
        Optional<String> packetName,
        String topic,
        RoutingId targetNodeRid,
        String spotId) {
        if (!flow.enabled(outcome)) {
            return;
        }
        flow.trace(new ZLinkMessageFlowEvent(
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
    private final java.util.concurrent.atomic.AtomicBoolean submitGate =
        new java.util.concurrent.atomic.AtomicBoolean();
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
            metadata);
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
            metadata.with(key, value));
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
            metadata);
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
            metadata.with(key, value));
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
            metadata.withAll(values));
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
            metadata);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectSameSpotWait(spotId);
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(
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
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
            .yieldCurrent(submit(replyType));
    }

}

final class ZLinkSpotDirectPublishCall implements ZLinkPublishCall {
    private final java.util.concurrent.atomic.AtomicBoolean submitGate =
        new java.util.concurrent.atomic.AtomicBoolean();
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
            outbound, spot, channelName, topic, payload, Optional.of(packetName), contentType, metadata);
    }

    @Override
    public ZLinkPublishCall metadata(String key, String value) {
        return new ZLinkSpotDirectPublishCall(
            outbound, spot, channelName, topic, payload, packetName, contentType, metadata.with(key, value));
    }

    @Override
    public ZLinkPublishCall metadata(Map<String, String> values) {
        return new ZLinkSpotDirectPublishCall(
            outbound, spot, channelName, topic, payload, packetName, contentType, metadata.withAll(values));
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
