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
        String errorMessage,
        String meshName,
        String targetRid) {
    public ZLinkDispatchFailure(
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
        this(
                surface,
                messageKind,
                reason,
                action,
                packetName,
                channelName,
                topic,
                spotId,
                actorId,
                sourceRid,
                correlationId,
                errorType,
                errorMessage,
                null,
                null);
    }
}
