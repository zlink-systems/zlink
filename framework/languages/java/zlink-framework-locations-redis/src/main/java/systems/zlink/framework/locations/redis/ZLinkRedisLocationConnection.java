package systems.zlink.framework.locations.redis;

import io.lettuce.core.RedisClient;
import io.lettuce.core.RedisURI;
import io.lettuce.core.ScriptOutputType;
import io.lettuce.core.api.StatefulRedisConnection;
import io.lettuce.core.api.async.RedisAsyncCommands;
import io.lettuce.core.codec.RedisCodec;
import io.lettuce.core.codec.StringCodec;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.function.Function;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/**
 * Redis connection wrapper shared by the Location Store providers.
 *
 * <p>Parameterized over the value type {@code V} so callers that only ever
 * exchange text (the dedicated Lua paths in {@link ZLinkRedisLocationRepository})
 * can keep a plain {@link StringCodec}, while callers that must carry
 * 8-bit-clean opaque bytes through {@code EVAL} ARGV without a base64
 * sub-layer (the opaque record store, the relocation blob store) use a
 * byte[] value codec. See {@link #forStrings} and {@link #forBytes}.</p>
 */
final class ZLinkRedisLocationConnection<V> {
    private final RedisURI redisUri;
    private final RedisClient client;
    private final String schemaKey;
    private final RedisCodec<String, V> codec;
    private final Function<String, V> literal;
    private CompletableFuture<StatefulRedisConnection<String, V>> connection;
    private CompletableFuture<Void> schemaReady;
    private CompletionStage<Void> closeStage;
    private boolean closed;

    private ZLinkRedisLocationConnection(
        RedisURI redisUri,
        String schemaKey,
        RedisCodec<String, V> codec,
        Function<String, V> literal) {
        this.redisUri = redisUri;
        this.schemaKey = schemaKey;
        this.codec = codec;
        this.literal = literal;
        this.client = RedisClient.create(redisUri);
    }

    static ZLinkRedisLocationConnection<String> forStrings(
        ZLinkRedisLocationOptions options,
        String schemaKey) {
        return forStrings(options.redisUri(), schemaKey);
    }

    static ZLinkRedisLocationConnection<String> forStrings(RedisURI redisUri) {
        return forStrings(redisUri, null);
    }

    static ZLinkRedisLocationConnection<String> forStrings(
        RedisURI redisUri,
        String schemaKey) {
        return new ZLinkRedisLocationConnection<>(
            redisUri, schemaKey, StringCodec.UTF8, value -> value);
    }

    static ZLinkRedisLocationConnection<byte[]> forBytes(
        ZLinkRedisLocationOptions options,
        String schemaKey) {
        return forBytes(options.redisUri(), schemaKey);
    }

    static ZLinkRedisLocationConnection<byte[]> forBytes(RedisURI redisUri) {
        return forBytes(redisUri, null);
    }

    static ZLinkRedisLocationConnection<byte[]> forBytes(
        RedisURI redisUri,
        String schemaKey) {
        return new ZLinkRedisLocationConnection<>(
            redisUri,
            schemaKey,
            ZLinkRedisStringByteArrayCodec.INSTANCE,
            value -> value.getBytes(StandardCharsets.UTF_8));
    }

    CompletionStage<RedisAsyncCommands<String, V>> commands() {
        return connection()
            .thenCompose(this::verifySchema)
            .thenApply(StatefulRedisConnection::async);
    }

    synchronized CompletionStage<Void> closeAsync() {
        if (closeStage != null) {
            return closeStage;
        }
        closed = true;
        CompletableFuture<StatefulRedisConnection<String, V>> current = connection;
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

    private synchronized CompletionStage<StatefulRedisConnection<String, V>>
        verifySchema(
            StatefulRedisConnection<String, V> connected) {
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
                    literal.apply("location-authority-hybrid-v1"),
                    literal.apply("1"))
                .thenApply(result -> {
                    if (!"ready".equals(
                        text(result.getFirst()))) {
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

    private synchronized CompletionStage<StatefulRedisConnection<String, V>> connection() {
        if (closed) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("Redis location connection is closed."));
        }
        if (connection != null) {
            return connection;
        }

        CompletableFuture<StatefulRedisConnection<String, V>> created =
            client.connectAsync(codec, redisUri).toCompletableFuture();
        connection = created;
        created.whenComplete((ignored, failure) -> {
            if (failure != null) {
                clearFailedConnection(created);
            }
        });
        return created;
    }

    private synchronized void clearFailedConnection(
        CompletableFuture<StatefulRedisConnection<String, V>> failed) {
        if (connection == failed) {
            connection = null;
        }
    }

    private static String text(Object value) {
        if (value instanceof byte[] bytes) {
            return new String(bytes, StandardCharsets.UTF_8);
        }
        return String.valueOf(value);
    }
}
