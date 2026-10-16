package MyGame.channel;

import MyGame.*;

import io.lettuce.core.RedisClient;
import io.lettuce.core.api.StatefulRedisConnection;

import org.springframework.stereotype.Component;

import java.util.concurrent.CompletionStage;

@Component
public final class PlayerData implements AutoCloseable {
    private final RedisClient client;
    private final StatefulRedisConnection<String, String> connection;
    private final String prefix;

    public PlayerData(NodeConfig config) {
        prefix = config.keyPrefix() + Names.PLAYER_KEY;
        client = RedisClient.create("redis://" + config.redisEndpoint());
        try {
            connection = client.connect();
        } catch (RuntimeException error) {
            client.shutdown();
            throw error;
        }
    }

    public CompletionStage<Long> load(String id) {
        return connection
                .async()
                .get(prefix + id)
                .thenApply(s -> s == null ? 0L : Long.parseLong(s));
    }

    public CompletionStage<Void> save(String id, long gold) {
        return connection.async().set(prefix + id, Long.toString(gold)).thenApply(s -> null);
    }

    public void close() {
        try {
            connection.close();
        } finally {
            client.shutdown();
        }
    }
}
