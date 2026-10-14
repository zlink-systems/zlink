package MyGame;

public record NodeConfig(
        String id,
        String redisEndpoint,
        String keyPrefix,
        String meshEndpoint,
        String streamEndpoint,
        String httpEndpoint,
        String sessionEndpoint) {}
