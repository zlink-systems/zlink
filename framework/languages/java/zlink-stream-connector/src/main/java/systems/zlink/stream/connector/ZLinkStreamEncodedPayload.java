package systems.zlink.stream.connector;

import java.util.Map;
import systems.zlink.contracts.messaging.Message;

public record ZLinkStreamEncodedPayload(
    String packetName,
    Message payload,
    Map<String, String> metadata,
    ZLinkStreamCodec codec) {
    public ZLinkStreamEncodedPayload(
        String packetName,
        Message payload,
        Map<String, String> metadata) {
        this(packetName, payload, metadata, ZLinkStreamCodec.RAW);
    }
}
