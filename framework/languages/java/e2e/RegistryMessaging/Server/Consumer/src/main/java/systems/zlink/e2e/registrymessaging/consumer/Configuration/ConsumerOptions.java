package systems.zlink.e2e.registrymessaging.consumer.Configuration;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ConsumerOptions(
    String consumerName,
    String consumerMode,
    String providerEndpoints,
    int httpPort,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir) {
    public ConsumerOptions {
        required(consumerName, "consumer-name");
        required(consumerMode, "consumer-mode");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        required(logDir, "log-dir");
        providerEndpoints = providerEndpoints == null ? "" : providerEndpoints;
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
