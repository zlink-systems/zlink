package systems.zlink.framework.locations.redis;

import io.lettuce.core.RedisClient;
import io.lettuce.core.RedisURI;
import io.lettuce.core.api.StatefulRedisConnection;
import io.lettuce.core.api.async.RedisAsyncCommands;
import io.lettuce.core.codec.StringCodec;
import io.lettuce.core.ScriptOutputType;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkRedisLocationConnection {
    private final RedisURI redisUri;
    private final RedisClient client;
    private final String schemaKey;
    private CompletableFuture<StatefulRedisConnection<String, String>> connection;
    private CompletableFuture<Void> schemaReady;
    private CompletionStage<Void> closeStage;
    private boolean closed;

    ZLinkRedisLocationConnection(
        ZLinkRedisLocationOptions options,
        String schemaKey) {
        this(options.redisUri(), schemaKey);
    }

    ZLinkRedisLocationConnection(RedisURI redisUri) {
        this(redisUri, null);
    }

    private ZLinkRedisLocationConnection(
        RedisURI redisUri,
        String schemaKey) {
        this.redisUri = redisUri;
        this.schemaKey = schemaKey;
        this.client = RedisClient.create(redisUri);
    }

    CompletionStage<RedisAsyncCommands<String, String>> commands() {
        return connection()
            .thenCompose(this::verifySchema)
            .thenApply(StatefulRedisConnection::async);
    }

    synchronized CompletionStage<Void> closeAsync() {
        if (closeStage != null) {
            return closeStage;
        }
        closed = true;
        CompletableFuture<StatefulRedisConnection<String, String>> current = connection;
        connection = null;
        schemaReady = null;
        CompletionStage<Void> connectionClosed = current == null
            ? CompletableFuture.completedFuture(null)
            : current.handle((connected, failure) -> connected)
                .thenCompose(connected -> connected == null
                    ? CompletableFuture.completedFuture(null)
                    : connected.closeAsync());
        closeStage = connectionClosed
            .thenCompose(ignored -> client.shutdownAsync())
            .thenApply(ignored -> null);
        return closeStage;
    }

    private synchronized CompletionStage<StatefulRedisConnection<String, String>>
        verifySchema(
            StatefulRedisConnection<String, String> connected) {
        if (schemaKey == null) {
            return CompletableFuture.completedFuture(connected);
        }
        if (schemaReady == null) {
            schemaReady = connected.async()
                .<List<Object>>eval(
                    """
                    local format = redis.call(
                        'HGET', KEYS[1], 'format')
                    local epoch = redis.call(
                        'HGET', KEYS[1], 'epoch')
                    if redis.call('EXISTS', KEYS[1]) == 0 then
                        redis.call('HSET', KEYS[1],
                            'format', ARGV[1],
                            'epoch', ARGV[2])
                        return {'ready'}
                    end
                    if format == ARGV[1]
                        and epoch == ARGV[2] then
                        return {'ready'}
                    end
                    return {'incompatible',
                        format or '', epoch or ''}
                    """,
                    ScriptOutputType.MULTI,
                    new String[] {schemaKey},
                    "location-authority-hybrid-v1",
                    "1")
                .thenApply(result -> {
                    if (!"ready".equals(
                        String.valueOf(result.getFirst()))) {
                        throw new IllegalStateException(
                            "Redis location schema is incompatible: "
                                + result);
                    }
                    return (Void) null;
                })
                .toCompletableFuture();
        }
        return schemaReady.thenApply(ignored -> connected);
    }

    private synchronized CompletionStage<StatefulRedisConnection<String, String>> connection() {
        if (closed) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("Redis location connection is closed."));
        }
        if (connection != null) {
            return connection;
        }

        CompletableFuture<StatefulRedisConnection<String, String>> created =
            client.connectAsync(StringCodec.UTF8, redisUri).toCompletableFuture();
        connection = created;
        created.whenComplete((ignored, failure) -> {
            if (failure != null) {
                clearFailedConnection(created);
            }
        });
        return created;
    }

    private synchronized void clearFailedConnection(
        CompletableFuture<StatefulRedisConnection<String, String>> failed) {
        if (connection == failed) {
            connection = null;
        }
    }
}
