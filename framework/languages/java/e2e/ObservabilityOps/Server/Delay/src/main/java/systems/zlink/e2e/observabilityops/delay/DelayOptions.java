package systems.zlink.e2e.observabilityops.delay;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record DelayOptions(
    String delayEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir) {
    public DelayOptions {
        required(delayEndpoint, "delay-endpoint");
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
