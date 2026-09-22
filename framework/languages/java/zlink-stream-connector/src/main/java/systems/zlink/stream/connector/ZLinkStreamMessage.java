package systems.zlink.stream.connector;

import java.util.Map;

public record ZLinkStreamMessage<TPayload>(
        String packetName,
        TPayload payload,
        Map<String, String> metadata,
        String flowId,
        ZLinkFlowOrigin flowOrigin,
        String actorId)
        implements ZLinkStreamFlow {
    public ZLinkStreamMessage(String packetName, TPayload payload, Map<String, String> metadata) {
        this(packetName, payload, metadata, null, null, null);
    }

    public ZLinkStreamMessage(
            String packetName,
            TPayload payload,
            Map<String, String> metadata,
            String flowId,
            ZLinkFlowOrigin flowOrigin) {
        this(packetName, payload, metadata, flowId, flowOrigin, null);
    }
}
