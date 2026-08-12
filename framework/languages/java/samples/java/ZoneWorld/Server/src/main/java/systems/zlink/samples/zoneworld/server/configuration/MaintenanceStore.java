package systems.zlink.samples.zoneworld.server.configuration;

import io.lettuce.core.RedisClient;
import io.lettuce.core.api.StatefulRedisConnection;
import io.lettuce.core.api.sync.RedisCommands;
public final class MaintenanceStore implements AutoCloseable {
    private final RedisClient client;
    private final StatefulRedisConnection<String, String> connection;
    private final RedisCommands<String, String> redis;
    private final String keyPrefix;

    public MaintenanceStore(String endpoint, String keyPrefix) {
        String redisUri = endpoint.contains("://")
            ? endpoint
            : "redis://" + endpoint;
        client = RedisClient.create(redisUri);
        connection = client.connect();
        redis = connection.sync();
        this.keyPrefix = keyPrefix + "maintenance:";
    }

    public void set(String nodeId, boolean enabled) {
        redis.set(keyPrefix + nodeId, Boolean.toString(enabled));
    }

    public boolean get(String nodeId) {
        return Boolean.parseBoolean(redis.get(keyPrefix + nodeId));
    }

    @Override
    public void close() {
        connection.close();
        client.shutdown();
    }
}
