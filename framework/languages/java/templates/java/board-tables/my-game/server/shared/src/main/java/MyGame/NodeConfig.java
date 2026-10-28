package MyGame;

public record NodeConfig(
        String id,
        String redisEndpoint,
        String keyPrefix,
        String meshEndpoint,
        String streamEndpoint,
        int tableCapacity) {
    public NodeConfig {
        if (tableCapacity < 0)
            throw new IllegalArgumentException("Table capacity must be nonnegative");
    }
}
