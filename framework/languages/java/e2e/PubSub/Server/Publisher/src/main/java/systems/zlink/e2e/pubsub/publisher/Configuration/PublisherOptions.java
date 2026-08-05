package systems.zlink.e2e.pubsub.publisher.Configuration;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record PublisherOptions(
    String httpEndpoint,
    String publisherEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir) {
    public PublisherOptions {
        required(httpEndpoint, "http-endpoint");
        required(publisherEndpoint, "publisher-endpoint");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        required(logDir, "log-dir");
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("e2e." + name + " is required");
        }
    }
}
