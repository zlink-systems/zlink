package systems.zlink.e2e.spotservice.gateway;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record GatewayOptions(
    String gatewayRid, String gatewayHttpEndpoint, String routeEndpoint,
    String routeAEndpoint, String routeBEndpoint, String ingressAEndpoint,
    String spotEndpoint, boolean spotOnly, String redisLocationEndpoint,
    String locationKeyPrefix, String logDir) {
    public GatewayOptions {
        required(gatewayRid, "gateway-rid"); required(gatewayHttpEndpoint, "gateway-http-endpoint");
        required(routeEndpoint, "route-endpoint"); required(routeAEndpoint, "route-a-endpoint");
        required(routeBEndpoint, "route-b-endpoint"); required(ingressAEndpoint, "ingress-a-endpoint");
        required(spotEndpoint, "spot-endpoint"); required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix"); required(logDir, "log-dir");
    }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
