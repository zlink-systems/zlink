package systems.zlink.framework;

import java.util.Map;
import java.util.Optional;

public interface ZLinkMessageContext {
    Optional<String> meshName();

    Optional<String> channelName();

    String packetName();

    Optional<String> contentType();

    Map<String, String> metadata();

    Optional<String> correlationId();
}
