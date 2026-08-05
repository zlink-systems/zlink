package systems.zlink.e2e.registrymessaging.objectclient;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ObjectClientOptions(
    String clientRid,
    String routeEndpoint,
    String peerConnections,
    String serverWeight,
    int httpPort,
    String redisLocationEndpoint,
    String locationKeyPrefix) {
    public ObjectClientOptions {
        required(clientRid, "client-rid");
        required(routeEndpoint, "route-endpoint");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        peerConnections = peerConnections == null ? "" : peerConnections;
        serverWeight = serverWeight == null ? "" : serverWeight;
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("e2e." + name + " is required");
        }
    }
}
