package systems.zlink.framework.runtime.spots;

import java.util.Map;
import java.util.Optional;
import systems.zlink.framework.ZLinkMessageContext;

record ZLinkSpotActorSendHandlerContext(
    String packet,
    String content,
    Map<String, String> metadata) implements ZLinkMessageContext {
    ZLinkSpotActorSendHandlerContext(
        String packet,
        Map<String, String> metadata) {
        this(packet, null, metadata);
    }

    ZLinkSpotActorSendHandlerContext {
        metadata = metadata == null ? Map.of() : Map.copyOf(metadata);
    }

    @Override public Optional<String> meshName() { return Optional.empty(); }
    @Override public Optional<String> channelName() { return Optional.empty(); }
    @Override public String packetName() { return packet; }
    @Override public Optional<String> contentType() { return Optional.ofNullable(content); }
    @Override public Optional<String> correlationId() { return Optional.empty(); }
}

record ZLinkSpotActorRequestHandlerContext(
    String packet,
    String content,
    Map<String, String> metadata) implements ZLinkMessageContext {
    ZLinkSpotActorRequestHandlerContext(
        String packet,
        Map<String, String> metadata) {
        this(packet, null, metadata);
    }

    ZLinkSpotActorRequestHandlerContext {
        metadata = metadata == null ? Map.of() : Map.copyOf(metadata);
    }

    @Override public Optional<String> meshName() { return Optional.empty(); }
    @Override public Optional<String> channelName() { return Optional.empty(); }
    @Override public String packetName() { return packet; }
    @Override public Optional<String> contentType() { return Optional.ofNullable(content); }
    @Override public Optional<String> correlationId() { return Optional.empty(); }
}
