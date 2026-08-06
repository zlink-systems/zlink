package systems.zlink.e2e.observabilityops.play;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record PlayOptions(
    String nodeRid,
    String routeEndpoint,
    String routePeerEndpoint,
    String spotEndpoint,
    String delayEndpoint,
    String fanoutEndpoint,
    String httpEndpoint,
    String maintenanceEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir,
    long applicationVersion,
    int placementWeight,
    boolean automaticTopology) {
    public PlayOptions {
        required(nodeRid, "node-rid");
        required(routeEndpoint, "route-endpoint");
        routePeerEndpoint = optional(routePeerEndpoint, "");
        required(spotEndpoint, "spot-endpoint");
        required(delayEndpoint, "delay-endpoint");
        fanoutEndpoint = optional(fanoutEndpoint, "");
        required(httpEndpoint, "http-endpoint");
        maintenanceEndpoint = optional(maintenanceEndpoint, "");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        required(logDir, "log-dir");
        if (applicationVersion < 0) {
            throw new IllegalArgumentException("e2e.application-version must not be negative");
        }
        if (placementWeight < 0) {
            throw new IllegalArgumentException("e2e.placement-weight must not be negative");
        }
    }

    private static String optional(String value, String fallback) {
        return value == null ? fallback : value;
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("e2e." + name + " is required");
        }
    }
}
