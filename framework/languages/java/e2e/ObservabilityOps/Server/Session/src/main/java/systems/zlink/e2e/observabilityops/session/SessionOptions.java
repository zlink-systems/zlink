package systems.zlink.e2e.observabilityops.session;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record SessionOptions(
    String messageFlow,
    String routeEndpoint,
    String routeBEndpoint,
    String sessionRouteEndpoint,
    String sessionSpotEndpoint,
    String delayEndpoint,
    String streamEndpoint,
    String httpEndpoint,
    String sessionDrainSpot,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir) {
    public SessionOptions {
        messageFlow = optional(messageFlow, "on");
        if (!"on".equals(messageFlow) && !"off".equals(messageFlow)) {
            throw new IllegalArgumentException("e2e.message-flow must be on or off");
        }
        required(routeEndpoint, "route-endpoint");
        routeBEndpoint = optional(routeBEndpoint, "");
        required(sessionRouteEndpoint, "session-route-endpoint");
        required(sessionSpotEndpoint, "session-spot-endpoint");
        required(delayEndpoint, "delay-endpoint");
        required(streamEndpoint, "stream-endpoint");
        required(httpEndpoint, "http-endpoint");
        sessionDrainSpot = optional(sessionDrainSpot, "");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        required(logDir, "log-dir");
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
