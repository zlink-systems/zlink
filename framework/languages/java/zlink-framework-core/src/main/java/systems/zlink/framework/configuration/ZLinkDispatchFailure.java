package systems.zlink.framework.configuration;

public record ZLinkDispatchFailure(
    ZLinkDispatchErrorSurface surface,
    ZLinkDispatchMessageKind messageKind,
    ZLinkDispatchErrorReason reason,
    ZLinkDispatchErrorAction action,
    String packetName,
    String channelName,
    String topic,
    String spotId,
    String actorId,
    String sourceRid,
    String correlationId,
    String errorType,
    String errorMessage) {
}
