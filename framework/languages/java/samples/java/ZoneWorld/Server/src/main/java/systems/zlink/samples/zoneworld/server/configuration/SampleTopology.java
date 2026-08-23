package systems.zlink.samples.zoneworld.server.configuration;

import java.io.IOException;
import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Objects;
import java.util.Properties;
import org.springframework.boot.context.properties.ConfigurationProperties;
@ConfigurationProperties("sample")
public record SampleTopology(
    String role,
    String nodeId,
    String meshEndpoint,
    String streamEndpoint,
    String redisEndpoint,
    String redisKeyPrefix,
    Boolean subscriberOnly,
    Boolean disableBots,
    Boolean allowEmptyZoneSet,
    String faultTickZone,
    String meshAdvertiseHost) {

    public boolean is(String expected) {
        return expected.equalsIgnoreCase(role);
    }

    public boolean isSubscriberOnly() { return Boolean.TRUE.equals(subscriberOnly); }
    public boolean botsDisabled() { return Boolean.TRUE.equals(disableBots); }
    public boolean allowsEmptyZoneSet() { return Boolean.TRUE.equals(allowEmptyZoneSet); }

    public static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: ZoneWorldServer --config <path>");
        }
        return args[1];
    }

    public void validateServer() {
        require(role, "sample.role");
        require(meshEndpoint, "sample.mesh-endpoint");
        require(redisEndpoint, "sample.redis-endpoint");
        require(redisKeyPrefix, "sample.redis-key-prefix");
        if (is("zone")) require(nodeId, "sample.node-id");
        if (is("gateway") || is("ops")) require(streamEndpoint, "sample.stream-endpoint");
        if (!is("zone") && !is("gateway") && !is("ops")) {
            throw new IllegalArgumentException("sample.role must be gateway, zone, or ops");
        }
    }

    private static void require(String value, String name) {
        if (Objects.requireNonNullElse(value, "").isBlank()) {
            throw new IllegalArgumentException(name + " is required");
        }
    }
}
