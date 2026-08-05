package systems.zlink.e2e.resiliencelifecycle.provider;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ProviderOptions(
    String providerRid,
    String apiEndpoint,
    String httpEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir) {
    public ProviderOptions {
        required(providerRid, "provider-rid");
        required(apiEndpoint, "api-endpoint");
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
