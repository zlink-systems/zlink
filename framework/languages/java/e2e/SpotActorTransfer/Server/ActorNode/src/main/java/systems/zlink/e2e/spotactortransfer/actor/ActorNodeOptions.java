package systems.zlink.e2e.spotactortransfer.actor;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ActorNodeOptions(
    String nodeRid, String meshEndpoint, String meshPeers, String streamEndpoint,
    String httpEndpoint, String redisLocationEndpoint, String locationKeyPrefix,
    String logDirectory, String scenario, boolean automaticTopology) {
    public ActorNodeOptions {
        required(nodeRid, "node-rid"); required(meshEndpoint, "mesh-endpoint");
        required(meshPeers, "mesh-peers"); required(streamEndpoint, "stream-endpoint");
        required(httpEndpoint, "http-endpoint"); required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix"); required(logDirectory, "log-directory");
        required(scenario, "scenario");
    }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
