package systems.zlink.framework.runtime.internal.diagnostics;

import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

/** Internal Spec 26 telemetry record. This type is not part of the public API. */
public record ZLinkMessageFlowEvent(
    ZLinkTraceEventId eventId,
    ZLinkMessageFlowOutcome phase,
    ZLinkMessageFlowResult outcome,
    ZLinkDispatchErrorSurface surface,
    ZLinkDispatchMessageKind messageKind,
    String packetName,
    String channelName,
    ZLinkChannelRouteKind channelRouteKind,
    String meshName,
    String topic,
    String correlationId,
    String sourceRid,
    String targetRid,
    String serverRid,
    String spotId,
    String instanceSpotType,
    ZLinkActivationState activationState,
    String actorId,
    Long messageSize,
    Double durationSeconds,
    ZLinkDispatchErrorReason errorReason,
    ZLinkDispatchErrorAction errorAction,
    String errorType,
    String errorMessage,
    String errorCauseType,
    String errorCauseMessage,
    String flowId,
    ZLinkFlowOrigin flowOrigin,
    Long sourceMeshGeneration) {

    public ZLinkMessageFlowEvent {
        if (eventId == null || outcome == null || surface == null || messageKind == null) {
            throw new IllegalArgumentException(
                "eventId, outcome, surface and messageKind are required");
        }
        if (eventId == ZLinkTraceEventId.MESSAGE_FLOW && phase == null) {
            throw new IllegalArgumentException("message-flow events require a phase");
        }
        if (eventId == ZLinkTraceEventId.DISPATCH_ERROR
            && (phase != null || outcome != ZLinkMessageFlowResult.FAILED)) {
            throw new IllegalArgumentException(
                "dispatch-error events have no phase and require outcome=failed");
        }
        if (eventId == ZLinkTraceEventId.DISPATCH_ERROR
            && (errorReason == null || errorAction == null)) {
            throw new IllegalArgumentException(
                "dispatch-error events require reason and action");
        }
        if (eventId == ZLinkTraceEventId.MESSAGE_FLOW && errorAction != null) {
            throw new IllegalArgumentException(
                "message-flow events must not include a dispatch-error action");
        }
        if (eventId == ZLinkTraceEventId.MESSAGE_FLOW
            && messageKind == ZLinkDispatchMessageKind.PUBLISH) {
            throw new IllegalArgumentException(
                "logical multicast and classic fanout must not create message-flow events");
        }
        if (channelRouteKind != null && !"channel".equals(surface.traceName())) {
            throw new IllegalArgumentException(
                "channelRouteKind is only valid for the channel surface");
        }
        if ((flowId == null) != (flowOrigin == null)) {
            throw new IllegalArgumentException("flowId and flowOrigin must be present together");
        }
    }

    /** Compatibility constructor for ordinary message-flow processing points. */
    public ZLinkMessageFlowEvent(
        ZLinkMessageFlowOutcome phase,
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind messageKind,
        String packetName,
        String channelName,
        String topic,
        String correlationId,
        String sourceRid,
        String spotId,
        String actorId,
        Long messageSize) {
        this(
            ZLinkTraceEventId.MESSAGE_FLOW,
            phase,
            defaultOutcome(phase),
            surface,
            messageKind,
            packetName,
            channelName,
            defaultRouteKind(surface),
            null,
            topic,
            correlationId,
            sourceRid,
            null,
            null,
            spotId,
            null,
            null,
            actorId,
            messageSize,
            null,
            defaultReason(phase),
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null);
    }

    /** Compatibility constructor for flow-correlated processing points. */
    public ZLinkMessageFlowEvent(
        ZLinkMessageFlowOutcome phase,
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind messageKind,
        String packetName,
        String channelName,
        String topic,
        String correlationId,
        String sourceRid,
        String spotId,
        String actorId,
        Long messageSize,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action,
        String errorType,
        String errorMessage,
        String flowId,
        ZLinkFlowOrigin flowOrigin) {
        this(
            ZLinkTraceEventId.MESSAGE_FLOW, phase, defaultOutcome(phase), surface,
            messageKind, packetName, channelName, defaultRouteKind(surface), null,
            topic, correlationId, sourceRid, null, null, spotId, null, null, actorId,
            messageSize, null, reason == null ? defaultReason(phase) : reason, action,
            errorType, errorMessage, null, null, flowId, flowOrigin, null);
    }

    public static ZLinkMessageFlowEvent dispatchError(
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind messageKind,
        String packetName,
        String channelName,
        String topic,
        String correlationId,
        String sourceRid,
        String spotId,
        String actorId,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action,
        String errorType,
        String errorMessage) {
        return new ZLinkMessageFlowEvent(
            ZLinkTraceEventId.DISPATCH_ERROR,
            null,
            ZLinkMessageFlowResult.FAILED,
            surface,
            messageKind,
            packetName,
            channelName,
            defaultRouteKind(surface),
            null,
            topic,
            correlationId,
            sourceRid,
            null,
            null,
            spotId,
            null,
            null,
            actorId,
            null,
            null,
            reason,
            action,
            errorType,
            errorMessage,
            null,
            null,
            null,
            null,
            null);
    }

    public ZLinkMessageFlowEvent withFlow(String id, ZLinkFlowOrigin origin) {
        return copy(outcome, id, origin);
    }

    public ZLinkMessageFlowEvent withOutcome(ZLinkMessageFlowResult result) {
        return copy(result, flowId, flowOrigin);
    }

    private ZLinkMessageFlowEvent copy(
        ZLinkMessageFlowResult result,
        String id,
        ZLinkFlowOrigin origin) {
        return new ZLinkMessageFlowEvent(
            eventId, phase, result, surface, messageKind, packetName, channelName,
            channelRouteKind, meshName, topic, correlationId, sourceRid, targetRid,
            serverRid, spotId, instanceSpotType, activationState, actorId, messageSize,
            durationSeconds, errorReason, errorAction, errorType, errorMessage,
            errorCauseType, errorCauseMessage, id, origin, sourceMeshGeneration);
    }

    private static ZLinkMessageFlowResult defaultOutcome(ZLinkMessageFlowOutcome phase) {
        if (phase == ZLinkMessageFlowOutcome.BACKPRESSURED) {
            return ZLinkMessageFlowResult.BACKPRESSURED;
        }
        if (phase == ZLinkMessageFlowOutcome.DROPPED) {
            return ZLinkMessageFlowResult.DROPPED;
        }
        return ZLinkMessageFlowResult.SUCCEEDED;
    }

    private static ZLinkDispatchErrorReason defaultReason(ZLinkMessageFlowOutcome phase) {
        return phase == ZLinkMessageFlowOutcome.BACKPRESSURED
            ? ZLinkDispatchErrorReason.BACKPRESSURE
            : null;
    }

    private static ZLinkChannelRouteKind defaultRouteKind(
        ZLinkDispatchErrorSurface surface) {
        if (surface == ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL) {
            return ZLinkChannelRouteKind.ROUTE_MESH;
        }
        if (surface == ZLinkDispatchErrorSurface.CHANNEL) {
            return ZLinkChannelRouteKind.CLIENT_SERVER;
        }
        return null;
    }
}
