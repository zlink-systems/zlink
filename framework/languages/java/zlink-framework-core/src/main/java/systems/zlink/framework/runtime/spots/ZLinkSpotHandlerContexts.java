package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.ZLinkMessageContext;

import java.time.Instant;
import java.util.Map;
import java.util.Optional;

record ZLinkSpotActorSendHandlerContext(
        String packet, String content, Map<String, String> metadata, Instant receivedAt)
        implements ZLinkMessageContext {
    ZLinkSpotActorSendHandlerContext(String packet, Map<String, String> metadata) {
        this(packet, null, metadata, Instant.now());
    }

    ZLinkSpotActorSendHandlerContext(String packet, String content, Map<String, String> metadata) {
        this(packet, content, metadata, Instant.now());
    }

    ZLinkSpotActorSendHandlerContext {
        metadata = metadata == null ? Map.of() : Map.copyOf(metadata);
    }

    @Override
    public Optional<String> meshName() {
        return Optional.empty();
    }

    @Override
    public Optional<String> channelName() {
        return Optional.empty();
    }

    @Override
    public String packetName() {
        return packet;
    }

    @Override
    public Optional<String> contentType() {
        return Optional.ofNullable(content);
    }

    @Override
    public Optional<String> correlationId() {
        return Optional.empty();
    }
}

record ZLinkSpotActorRequestHandlerContext(
        String packet, String content, Map<String, String> metadata, Instant receivedAt)
        implements ZLinkMessageContext {
    ZLinkSpotActorRequestHandlerContext(String packet, Map<String, String> metadata) {
        this(packet, null, metadata, Instant.now());
    }

    ZLinkSpotActorRequestHandlerContext(
            String packet, String content, Map<String, String> metadata) {
        this(packet, content, metadata, Instant.now());
    }

    ZLinkSpotActorRequestHandlerContext {
        metadata = metadata == null ? Map.of() : Map.copyOf(metadata);
    }

    @Override
    public Optional<String> meshName() {
        return Optional.empty();
    }

    @Override
    public Optional<String> channelName() {
        return Optional.empty();
    }

    @Override
    public String packetName() {
        return packet;
    }

    @Override
    public Optional<String> contentType() {
        return Optional.ofNullable(content);
    }

    @Override
    public Optional<String> correlationId() {
        return Optional.empty();
    }
}
