package Configuration;
import systems.zlink.e2e.pubsub.publisher.Configuration;
import systems.zlink.e2e.pubsub.shared.Contracts;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record PublisherOptions(
    String httpEndpoint,
    String publisherEndpoint,
    Integer publisherPort,
    String routingId,
    String routingIdPrefix,
    String advertiseHost,
    String channelName,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir) {
    public PublisherOptions {
        required(httpEndpoint, "http-endpoint");
        if ((publisherEndpoint == null || publisherEndpoint.isBlank()) && publisherPort == null) {
            throw new IllegalArgumentException(
                "e2e.publisher-endpoint or e2e.publisher-port is required");
        }
        channelName = channelName == null || channelName.isBlank()
            ? Contracts.EVENT_CHANNEL
            : channelName;
        if (redisLocationEndpoint != null && !redisLocationEndpoint.isBlank()) {
            required(locationKeyPrefix, "location-key-prefix");
        }
        required(logDir, "log-dir");
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("e2e." + name + " is required");
        }
    }
}
