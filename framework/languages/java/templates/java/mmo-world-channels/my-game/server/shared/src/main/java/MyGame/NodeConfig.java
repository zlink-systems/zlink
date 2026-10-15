package MyGame;

import java.util.List;

public record NodeConfig(
        String id,
        String redisEndpoint,
        String keyPrefix,
        String meshEndpoint,
        String streamEndpoint,
        String worldId,
        List<String> zoneIds,
        int zoneCapacity,
        String httpEndpoint,
        List<WorldInfo> worlds) {}
