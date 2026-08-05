package systems.zlink.e2e.resiliencelifecycle.consumer;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ConsumerOptions(
    String httpEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir) {
    public ConsumerOptions {
        required(httpEndpoint, "http-endpoint");
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
