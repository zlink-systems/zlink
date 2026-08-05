package systems.zlink.e2e.spotservice.multinode;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record MultiNodeOptions(
    String nodeRid, String routeEndpoint, String routeAEndpoint, String routeBEndpoint,
    String spotEndpoint, String httpEndpoint, boolean spotOnly,
    String redisLocationEndpoint, String locationKeyPrefix, String logDir) {
    public MultiNodeOptions {
        required(nodeRid, "node-rid"); required(routeEndpoint, "route-endpoint");
        required(routeAEndpoint, "route-a-endpoint"); required(routeBEndpoint, "route-b-endpoint");
        required(spotEndpoint, "spot-endpoint"); required(httpEndpoint, "http-endpoint");
        required(redisLocationEndpoint, "redis-location-endpoint"); required(locationKeyPrefix, "location-key-prefix");
        required(logDir, "log-dir");
    }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
