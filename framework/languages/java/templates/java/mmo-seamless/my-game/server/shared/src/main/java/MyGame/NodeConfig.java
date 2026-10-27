package MyGame;

public record NodeConfig(
        String id,
        String redisEndpoint,
        String keyPrefix,
        String meshEndpoint,
        String streamEndpoint,
        java.util.List<String> zoneIds,
        int zoneCapacity) {}
