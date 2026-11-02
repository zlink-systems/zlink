package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;

import java.time.Instant;
import java.util.Map;
import java.util.Optional;

record ZLinkSpotSendHandlerContext(
        String packet, String content, Map<String, String> metadata, Instant receivedAt)
        implements ZLinkMessageContext {
    ZLinkSpotSendHandlerContext(String packet, String content, Map<String, String> metadata) {
        this(packet, content, metadata, Instant.now());
    }

    ZLinkSpotSendHandlerContext {
        metadata = Map.copyOf(metadata);
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

record ZLinkSpotRequestHandlerContext(
        String packet, String content, Map<String, String> metadata, Instant receivedAt)
        implements ZLinkMessageContext {
    ZLinkSpotRequestHandlerContext(String packet, String content, Map<String, String> metadata) {
        this(packet, content, metadata, Instant.now());
    }

    ZLinkSpotRequestHandlerContext {
        metadata = Map.copyOf(metadata);
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

record ZLinkSpotPublishHandlerContext(
        String channel,
        String packet,
        String topic,
        String content,
        Optional<String> source,
        Map<String, String> metadata,
        Instant receivedAt)
        implements ZLinkPublishMessageContext {
    ZLinkSpotPublishHandlerContext(
            String channel,
            String packet,
            String topic,
            String content,
            Optional<String> source,
            Map<String, String> metadata) {
        this(channel, packet, topic, content, source, metadata, Instant.now());
    }

    ZLinkSpotPublishHandlerContext {
        source = source == null ? Optional.empty() : source;
        metadata = Map.copyOf(metadata);
    }

    @Override
    public Optional<String> meshName() {
        return Optional.empty();
    }

    @Override
    public Optional<String> channelName() {
        return Optional.ofNullable(channel).filter(value -> !value.isBlank());
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
