package systems.zlink.stream.connector;

import java.util.Map;

public record ZLinkStreamMessage<TPayload>(
        String packetName, TPayload payload, Map<String, String> metadata, String actorId) {
    public ZLinkStreamMessage(String packetName, TPayload payload, Map<String, String> metadata) {
        this(packetName, payload, metadata, null);
    }
}
