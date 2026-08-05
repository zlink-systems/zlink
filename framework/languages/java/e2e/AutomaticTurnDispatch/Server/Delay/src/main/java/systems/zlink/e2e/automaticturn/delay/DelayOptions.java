package systems.zlink.e2e.automaticturn.delay;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record DelayOptions(
    String delayEndpoint, String redisLocationEndpoint, String locationKeyPrefix, String logDirectory) {
    public DelayOptions {
        required(delayEndpoint, "delay-endpoint");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        required(logDirectory, "log-directory");
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
