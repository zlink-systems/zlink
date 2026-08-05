package systems.zlink.samples.bingo.server.matchmaking;

import io.lettuce.core.RedisClient;
import io.lettuce.core.ScriptOutputType;
import io.lettuce.core.api.StatefulRedisConnection;
import java.util.Base64;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.samples.bingo.server.configuration.SampleTopology;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class RedisBingoMatchReservationStore implements AutoCloseable {
    private static final String RESERVE_SCRIPT = """
        local key = KEYS[1]
        local actor = ARGV[1]
        local room = ARGV[2]
        local settings = ARGV[3]
        local required = tonumber(ARGV[4])
        local current = redis.call('HGET', key, 'RoomId')
        if not current then
          redis.call('HMSET', key, 'RoomId', room, 'Settings', settings,
            'ReservedActorIds', actor, 'RequiredPlayers', required)
          return {room, settings}
        end
        local actors = redis.call('HGET', key, 'ReservedActorIds') or ''
        local currentSettings = redis.call('HGET', key, 'Settings')
        if string.find('|' .. actors .. '|', '|' .. actor .. '|', 1, true) then
          return {current, currentSettings}
        end
        local count = 0
        for _ in string.gmatch(actors, '[^|]+') do count = count + 1 end
        if count >= required then
          redis.call('HMSET', key, 'RoomId', room, 'Settings', settings,
            'ReservedActorIds', actor, 'RequiredPlayers', required)
          return {room, settings}
        end
        redis.call('HSET', key, 'ReservedActorIds', actors .. '|' .. actor)
        return {current, currentSettings}
        """;

    private final String keyPrefix;
    private final RedisClient client;
    private final StatefulRedisConnection<String, String> connection;

    public RedisBingoMatchReservationStore(SampleTopology topology) {
        keyPrefix = topology.redisKeyPrefix();
        client = RedisClient.create(redisUri(topology.redisEndpoint()));
        connection = client.connect();
    }

    private static String redisUri(String endpoint) {
        return endpoint.contains("://") ? endpoint : "redis://" + endpoint;
    }

    public CompletionStage<Messages.ReserveBingoRoomRes> reserve(
        Messages.ReserveBingoRoomReq request) {
        if (!"two-player".equals(request.getMode())) {
            return CompletableFuture.failedFuture(new IllegalArgumentException(
                "unsupported bingo mode: " + request.getMode()));
        }
        if (request.getActorId().isBlank() || request.getLevelBucket().isBlank()) {
            return CompletableFuture.failedFuture(new IllegalArgumentException(
                "actor id and level bucket are required"));
        }
        String roomId = "bingo-room-" +
            UUID.randomUUID().toString().replace("-", "");
        Messages.BingoRoomSettingsPayload settings =
            Messages.BingoRoomSettingsPayload.newBuilder()
                .setRoomName("Bingo Room " + roomId.substring(roomId.length() - 6))
                .setMode(request.getMode())
                .setRequiredPlayers(2)
                .setMaxDrawNumber(15)
                .setPurpose("Game")
                .build();
        String encoded = Base64.getEncoder().encodeToString(settings.toByteArray());
        return connection.async().eval(
                RESERVE_SCRIPT,
                ScriptOutputType.MULTI,
                new String[] {keyPrefix + "match:" + request.getLevelBucket()
                    + ":" + request.getMode()},
                request.getActorId(), roomId, encoded, "2")
            .toCompletableFuture()
            .thenApply(value -> {
                @SuppressWarnings("unchecked")
                List<Object> result = (List<Object>) value;
                try {
                    String selectedRoom = result.get(0).toString();
                    Messages.BingoRoomSettingsPayload selectedSettings =
                        Messages.BingoRoomSettingsPayload.parseFrom(
                            Base64.getDecoder().decode(result.get(1).toString()));
                    return BingoMessages.reserveBingoRoomRes(
                        selectedRoom, selectedSettings);
                } catch (Exception failure) {
                    throw new IllegalStateException(
                        "invalid Redis matchmaking reservation", failure);
                }
            });
    }

    @Override
    public void close() {
        connection.close();
        client.shutdown();
    }
}
