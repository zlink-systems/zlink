package systems.zlink.stream.connector;

import java.util.Map;

/** Mutable request metadata immediately before transmission. */
public final class ZLinkStreamRequestSendingContext {
    private final String requestPacketName;
    private final String actorId;
    private final Map<String, String> metadata;

    ZLinkStreamRequestSendingContext(
            String requestPacketName, String actorId, Map<String, String> metadata) {
        this.requestPacketName = requestPacketName;
        this.actorId = actorId;
        this.metadata = metadata;
    }

    public String requestPacketName() {
        return requestPacketName;
    }

    public String actorId() {
        return actorId;
    }

    public void setMetadata(String key, String value) {
        metadata.put(key, value);
    }
}
