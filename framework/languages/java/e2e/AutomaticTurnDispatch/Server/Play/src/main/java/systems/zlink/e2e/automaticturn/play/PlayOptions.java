package systems.zlink.e2e.automaticturn.play;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record PlayOptions(
    String nodeRid, String routeEndpoint, String routePeerEndpoint,
    String delayEndpoint, String observabilityFanoutEndpoint,
    String httpEndpoint, String redisLocationEndpoint, String locationKeyPrefix, String logDirectory) {
    public PlayOptions {
        required(nodeRid, "node-rid");
        required(routeEndpoint, "route-endpoint"); routePeerEndpoint = optional(routePeerEndpoint, "");
        required(delayEndpoint, "delay-endpoint");
        observabilityFanoutEndpoint = optional(observabilityFanoutEndpoint, "");
        required(httpEndpoint, "http-endpoint"); required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix"); required(logDirectory, "log-directory");
    }
    private static String optional(String value, String fallback) { return value == null ? fallback : value; }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
