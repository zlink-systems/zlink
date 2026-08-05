package systems.zlink.framework.configuration;

import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

public record ZLinkMessageFlowEvent(
    ZLinkMessageFlowOutcome outcome,
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
    ZLinkDispatchErrorReason errorReason,
    ZLinkDispatchErrorAction errorAction,
    String errorType,
    String errorMessage,
    String flowId,
    ZLinkFlowOrigin flowOrigin) {
    public ZLinkMessageFlowEvent {
        if ((flowId == null) != (flowOrigin == null)) {
            throw new IllegalArgumentException("flowId and flowOrigin must be present together");
        }
    }

    public ZLinkMessageFlowEvent(
        ZLinkMessageFlowOutcome outcome,
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
        ZLinkDispatchErrorReason errorReason,
        ZLinkDispatchErrorAction errorAction,
        String errorType,
        String errorMessage) {
        this(outcome, surface, messageKind, packetName, channelName, topic,
            correlationId, sourceRid, spotId, actorId, messageSize, errorReason,
            errorAction, errorType, errorMessage, null, null);
    }
    public ZLinkMessageFlowEvent(
        ZLinkMessageFlowOutcome outcome,
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
        this(outcome, surface, messageKind, packetName, channelName, topic,
            correlationId, sourceRid, spotId, actorId, messageSize,
            null, null, null, null, null, null);
    }

    public ZLinkMessageFlowEvent withFlow(String id, ZLinkFlowOrigin origin) {
        return new ZLinkMessageFlowEvent(outcome, surface, messageKind, packetName,
            channelName, topic, correlationId, sourceRid, spotId, actorId,
            messageSize, errorReason, errorAction, errorType, errorMessage, id, origin);
    }
}
