package systems.zlink.framework.runtime.internal.diagnostics;

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
