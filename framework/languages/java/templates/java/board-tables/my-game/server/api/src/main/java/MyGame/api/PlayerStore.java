package MyGame.api;

import MyGame.CommitRound;
import MyGame.Rules;

import io.lettuce.core.*;
import io.lettuce.core.api.StatefulRedisConnection;

import java.util.HashSet;
import java.util.concurrent.CompletionStage;

public final class PlayerStore implements AutoCloseable {
    private static final String LOAD_PLAYER =
            """
            local chips = redis.call('GET', KEYS[1])
            if not chips then
                redis.call('SET', KEYS[1], ARGV[1])
                return ARGV[1]
            end
            return chips
            """;

    private static final String COMMIT_ROUND =
            """
            if redis.call('EXISTS', KEYS[1]) == 1 then
                return 0
            end
            local writes = {KEYS[1], '1'}
            for i = 2, #KEYS do
                writes[#writes + 1] = KEYS[i]
                writes[#writes + 1] = ARGV[i - 1]
            end
            redis.call('MSET', unpack(writes))
            return 1
            """;

    private final RedisClient client;
    private final StatefulRedisConnection<String, String> connection;
    private final String prefix;

    PlayerStore(String endpoint, String prefix) {
        this.prefix = prefix;
        client = RedisClient.create(
                RedisURI.create(endpoint.contains("://") ? endpoint : "redis://" + endpoint));
        try {
            connection = client.connect();
        } catch (RuntimeException failure) {
            client.shutdown();
            throw failure;
        }
    }

    CompletionStage<Long> loadPlayer(String playerId) {
        return connection
                .async()
                .<String>eval(
                        LOAD_PLAYER,
                        ScriptOutputType.VALUE,
                        new String[] {playerKey(playerId)},
                        Long.toString(Rules.INITIAL_CHIPS))
                .thenApply(Long::parseLong);
    }

    CompletionStage<Void> commitRound(CommitRound round) {
        if (round.tableId() == null
                || round.tableId().isBlank()
                || round.handNumber() == 0
                || !Rules.isContractInteger(round.handNumber())
                || round.results() == null
                || round.results().size() < Rules.MIN_PLAYERS
                || round.results().size() > Rules.SEATS)
            throw new IllegalArgumentException(
                    "Round identity and results for all seats are required");

        var keys = new String[round.results().size() + 1];
        var chips = new String[round.results().size()];
        keys[0] = prefix + "rounds:" + round.handNumber() + ":" + round.tableId();
        var players = new HashSet<String>();
        for (int i = 0; i < chips.length; i++) {
            var result = round.results().get(i);
            if (result == null
                    || !players.add(result.playerId())
                    || !Rules.isContractInteger(result.chips()))
                throw new IllegalArgumentException(
                        "Round results require unique players and valid balances");
            keys[i + 1] = playerKey(result.playerId());
            chips[i] = Long.toString(result.chips());
        }
        // The script checks the round key and writes every balance atomically on the shared connection.
        return connection
                .async()
                .<Long>eval(COMMIT_ROUND, ScriptOutputType.INTEGER, keys, chips)
                .thenAccept(ignored -> {});
    }

    private String playerKey(String playerId) {
        if (playerId == null || playerId.isBlank())
            throw new IllegalArgumentException("Player ID is required");
        return prefix + "players:" + playerId;
    }

    public void close() {
        client.shutdown();
    }
}
