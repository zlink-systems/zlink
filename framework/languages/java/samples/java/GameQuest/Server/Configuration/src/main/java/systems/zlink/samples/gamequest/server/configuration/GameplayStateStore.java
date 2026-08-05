package systems.zlink.samples.gamequest.server.configuration;

import io.lettuce.core.RedisClient;
import io.lettuce.core.api.StatefulRedisConnection;
import io.lettuce.core.api.sync.RedisCommands;
import java.util.List;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class GameplayStateStore implements AutoCloseable {
    private final RedisClient client;
    private final StatefulRedisConnection<String, String> connection;
    private final RedisCommands<String, String> redis;
    private final String keyPrefix;

    public GameplayStateStore(SampleTopology topology) {
        SampleTopology.Location location = topology.location();
        keyPrefix = location.redisKeyPrefix() + "gamequest:gameplay:";
        client = RedisClient.create(redisUri(location.redisEndpoint()));
        connection = client.connect();
        redis = connection.sync();
    }

    public void record(Messages.GameplayMsg event) {
        if (redis.sadd(recordedEventsKey(event.playerId()), event.eventId()) == 0) {
            return;
        }
        switch (event.eventType()) {
            case "kill" -> increment(killsKey(event.playerId()), event.value(), event.count());
            case "collect" -> increment(itemsKey(event.playerId()), event.value(), event.count());
            case "mission" -> redis.sadd(missionsKey(event.playerId()), event.value());
            case "feature" -> redis.sadd(featuresKey(event.playerId()), event.value());
            case "area" -> redis.sadd(areasKey(event.playerId()), event.value());
            default -> {
            }
        }
    }

    public void incrementKill(String playerId, String monsterId, int count) {
        increment(killsKey(playerId), monsterId, count);
    }

    public int killCount(String playerId, String monsterId) {
        return count(redis.hget(killsKey(playerId), monsterId));
    }

    public int itemCount(String playerId, String itemId) {
        return count(redis.hget(itemsKey(playerId), itemId));
    }

    public Messages.GetGameplaySnapshotRes snapshot(String playerId) {
        List<Messages.KillCountSnapshot> kills = redis.hgetall(killsKey(playerId)).entrySet().stream()
            .map(entry -> new Messages.KillCountSnapshot(
                entry.getKey(), null, count(entry.getValue())))
            .toList();
        List<Messages.ItemCountSnapshot> items = redis.hgetall(itemsKey(playerId)).entrySet().stream()
            .map(entry -> new Messages.ItemCountSnapshot(entry.getKey(), count(entry.getValue())))
            .toList();
        List<String> missions = redis.smembers(missionsKey(playerId)).stream().sorted().toList();
        List<String> features = redis.smembers(featuresKey(playerId)).stream().sorted().toList();
        List<String> areas = redis.smembers(areasKey(playerId)).stream().sorted().toList();
        long snapshotVersion = kills.stream().mapToLong(Messages.KillCountSnapshot::count).sum()
            + items.stream().mapToLong(Messages.ItemCountSnapshot::count).sum()
            + missions.size()
            + features.size()
            + areas.size();
        return new Messages.GetGameplaySnapshotRes(
            playerId, kills, items, missions, features, areas, snapshotVersion);
    }

    @Override
    public void close() {
        connection.close();
        client.shutdown();
    }

    private void increment(String key, String field, int count) {
        redis.hincrby(key, field, count);
    }

    private String killsKey(String playerId) {
        return keyPrefix + playerId + ":kills";
    }

    private String itemsKey(String playerId) {
        return keyPrefix + playerId + ":items";
    }

    private String missionsKey(String playerId) {
        return keyPrefix + playerId + ":missions";
    }

    private String featuresKey(String playerId) {
        return keyPrefix + playerId + ":features";
    }

    private String areasKey(String playerId) {
        return keyPrefix + playerId + ":areas";
    }

    private String recordedEventsKey(String playerId) {
        return keyPrefix + playerId + ":recorded-events";
    }

    private static int count(String value) {
        return value == null ? 0 : Integer.parseInt(value);
    }

    private static String redisUri(String endpoint) {
        return endpoint.startsWith("redis://") ? endpoint : "redis://" + endpoint;
    }
}
