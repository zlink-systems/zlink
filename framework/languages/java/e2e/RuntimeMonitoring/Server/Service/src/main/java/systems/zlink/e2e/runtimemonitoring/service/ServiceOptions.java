package systems.zlink.e2e.runtimemonitoring.service;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ServiceOptions(
    String routingId, String apiEndpoint, String handshakeEndpoint,
    String meshEndpoint, String meshPeerEndpoint, String httpEndpoint,
    boolean enableHandshake, boolean enableSpot,
    String redisLocationEndpoint, String locationKeyPrefix, String logDirectory) {
    public ServiceOptions {
        required(routingId, "routing-id"); required(apiEndpoint, "api-endpoint");
        required(httpEndpoint, "http-endpoint"); required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix"); required(logDirectory, "log-directory");
        handshakeEndpoint = optional(handshakeEndpoint); meshEndpoint = optional(meshEndpoint);
        meshPeerEndpoint = optional(meshPeerEndpoint);
        if (enableHandshake) required(handshakeEndpoint, "handshake-endpoint");
        if (enableSpot) required(meshEndpoint, "mesh-endpoint");
    }
    private static String optional(String value) { return value == null ? "" : value; }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
