package systems.zlink.e2e.automaticturn.session;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record SessionOptions(
    String messageFlowMode, String routeEndpoint, String routeBEndpoint,
    String sessionRouteEndpoint, String delayEndpoint,
    String streamEndpoint, String httpEndpoint, String sessionDrainSpotRid,
    String redisLocationEndpoint, String locationKeyPrefix, String logDirectory) {
    public SessionOptions {
        messageFlowMode = optional(messageFlowMode, "on");
        if (!"on".equals(messageFlowMode) && !"off".equals(messageFlowMode)) {
            throw new IllegalArgumentException("e2e.message-flow-mode must be on or off");
        }
        required(routeEndpoint, "route-endpoint"); routeBEndpoint = optional(routeBEndpoint, "");
        required(sessionRouteEndpoint, "session-route-endpoint");
        required(delayEndpoint, "delay-endpoint"); required(streamEndpoint, "stream-endpoint");
        required(httpEndpoint, "http-endpoint"); sessionDrainSpotRid = optional(sessionDrainSpotRid, "");
        required(redisLocationEndpoint, "redis-location-endpoint"); required(locationKeyPrefix, "location-key-prefix");
        required(logDirectory, "log-directory");
    }
    private static String optional(String value, String fallback) { return value == null ? fallback : value; }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
