package MyGame;

import java.util.List;

public record NodeConfig(
        String id,
        String redisEndpoint,
        String keyPrefix,
        String meshEndpoint,
        String streamEndpoint,
        int townCapacity,
        int dungeonCapacity,
        List<String> townIds) {}
