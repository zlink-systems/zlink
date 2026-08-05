package systems.zlink.framework.runtime.spots;

import java.util.Map;
import java.util.Optional;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.ZLinkMessageContext;

record ZLinkSpotSendHandlerContext(
    String packet,
    String content,
    Map<String, String> metadata) implements ZLinkMessageContext {
    ZLinkSpotSendHandlerContext {
        metadata = Map.copyOf(metadata);
    }

    @Override public Optional<String> meshName() { return Optional.empty(); }
    @Override public Optional<String> channelName() { return Optional.empty(); }
    @Override public String packetName() { return packet; }
    @Override public Optional<String> contentType() { return Optional.ofNullable(content); }
    @Override public Optional<String> correlationId() { return Optional.empty(); }
}

record ZLinkSpotRequestHandlerContext(
    String packet,
    String content,
    Map<String, String> metadata) implements ZLinkMessageContext {
    ZLinkSpotRequestHandlerContext {
        metadata = Map.copyOf(metadata);
    }

    @Override public Optional<String> meshName() { return Optional.empty(); }
    @Override public Optional<String> channelName() { return Optional.empty(); }
    @Override public String packetName() { return packet; }
    @Override public Optional<String> contentType() { return Optional.ofNullable(content); }
    @Override public Optional<String> correlationId() { return Optional.empty(); }
}

record ZLinkSpotPublishHandlerContext(
    String channel,
    String packet,
    String topic,
    String content,
    Optional<String> source,
    Map<String, String> metadata) implements ZLinkPublishMessageContext {
    ZLinkSpotPublishHandlerContext {
        source = source == null ? Optional.empty() : source;
        metadata = Map.copyOf(metadata);
    }

    @Override public Optional<String> meshName() { return Optional.empty(); }
    @Override public Optional<String> channelName() {
        return Optional.ofNullable(channel).filter(value -> !value.isBlank());
    }
    @Override public String packetName() { return packet; }
    @Override public Optional<String> contentType() { return Optional.ofNullable(content); }
    @Override public Optional<String> correlationId() { return Optional.empty(); }
}
