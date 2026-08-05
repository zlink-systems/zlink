package systems.zlink.samples.gamequest.server.configuration;

import com.fasterxml.jackson.databind.JavaType;
import com.fasterxml.jackson.databind.ObjectMapper;
import io.lettuce.core.RedisClient;
import io.lettuce.core.api.StatefulRedisConnection;
import io.lettuce.core.api.sync.RedisCommands;
import java.util.ArrayList;
import java.util.List;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class RedisSampleStore implements AutoCloseable {
    private final ObjectMapper json = new ObjectMapper();
    private final RedisClient client;
    private final StatefulRedisConnection<String, String> connection;
    private final RedisCommands<String, String> redis;

    private final String keyPrefix;

    public RedisSampleStore(SampleTopology topology) {
        SampleTopology.Location location = topology.location();
        keyPrefix = location.redisKeyPrefix();
        client = RedisClient.create(redisUri(location.redisEndpoint()));
        connection = client.connect();
        redis = connection.sync();
    }

    public void bind(String playerId, String apiName) {
        redis.sadd(key("binding-history"), playerId + ":" + apiName);
        redis.sadd(key("active-bindings"), playerId);
    }

    public void unbind(String playerId) {
        redis.srem(key("active-bindings"), playerId);
    }

    public List<String> bindingHistory() {
        return redis.smembers(key("binding-history")).stream().sorted().toList();
    }

    public List<String> activeBindings() {
        return redis.smembers(key("active-bindings")).stream().sorted().toList();
    }

    public void writeProjection(String playerId, List<Messages.QuestProgress> projection) {
        redis.set(key("projection:" + playerId), writeJson(projection));
    }

    public List<Messages.QuestProgress> readProjection(String playerId) {
        String value = redis.get(key("projection:" + playerId));
        if (value == null || value.isBlank()) {
            return List.of();
        }
        JavaType type = json.getTypeFactory()
            .constructCollectionType(List.class, Messages.QuestProgress.class);
        return readJson(value, type);
    }

    public void appendQuestEvents(List<Messages.StoredQuestEvent> events) {
        if (events.isEmpty()) {
            return;
        }
        redis.rpush(key("quest-events"), events.stream().map(this::writeJson).toArray(String[]::new));
    }

    public List<Messages.StoredQuestEvent> readQuestEvents() {
        return redis.lrange(key("quest-events"), 0, -1)
            .stream()
            .map(value -> readJson(value, Messages.StoredQuestEvent.class))
            .toList();
    }

    public void recordDeduplicatedEvent(String eventId) {
        redis.sadd(key("deduplicated-events"), eventId);
    }

    public List<String> deduplicatedEvents() {
        return redis.smembers(key("deduplicated-events")).stream().sorted().toList();
    }

    @Override
    public void close() {
        connection.close();
        client.shutdown();
    }

    private String key(String name) {
        return keyPrefix + "gamequest:" + name;
    }

    private String writeJson(Object value) {
        try {
            return json.writeValueAsString(value);
        } catch (Exception ex) {
            throw new IllegalStateException("Could not serialize sample store value.", ex);
        }
    }

    private <T> T readJson(String value, Class<T> type) {
        try {
            return json.readValue(value, type);
        } catch (Exception ex) {
            throw new IllegalStateException("Could not deserialize sample store value.", ex);
        }
    }

    private <T> T readJson(String value, JavaType type) {
        try {
            return json.readValue(value, type);
        } catch (Exception ex) {
            throw new IllegalStateException("Could not deserialize sample store value.", ex);
        }
    }

    private static String redisUri(String endpoint) {
        return endpoint.startsWith("redis://") ? endpoint : "redis://" + endpoint;
    }

}
