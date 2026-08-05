package systems.zlink.e2e.automaticturn.shared;

import io.lettuce.core.RedisClient;
import java.util.List;

public final class PersistentRoomEvents implements AutoCloseable {
    private final RedisClient client;
    private final io.lettuce.core.api.StatefulRedisConnection<String, String> connection;
    private final String prefix;

    public PersistentRoomEvents(String endpoint, String prefix) {
        String uri = endpoint.contains("://") ? endpoint : "redis://" + endpoint;
        client = RedisClient.create(uri);
        connection = client.connect();
        this.prefix = prefix;
    }

    public synchronized List<String> replay(String spotRid) {
        return connection.sync().lrange(key(spotRid), 0, -1);
    }

    public synchronized List<String> appendAndReplay(String spotRid, String value) {
        connection.sync().rpush(key(spotRid), value);
        return replay(spotRid);
    }

    private String key(String spotRid) {
        return prefix + ":business-events:" + spotRid;
    }

    @Override
    public void close() {
        connection.close();
        client.shutdown();
    }
}
