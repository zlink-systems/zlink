package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import io.lettuce.core.ScriptOutputType;
import io.lettuce.core.api.StatefulRedisConnection;
import io.lettuce.core.api.async.RedisAsyncCommands;
import io.lettuce.core.output.NestedMultiOutput;
import io.lettuce.core.protocol.AsyncCommand;
import io.lettuce.core.protocol.Command;
import io.lettuce.core.protocol.CommandType;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.locationprovider.ZLinkBlobReference;
import systems.zlink.framework.locationprovider.ZLinkBlobStored;

import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.concurrent.CompletableFuture;

final class ZLinkRedisRelocationBlobBoundTest {
    @ParameterizedTest
    @ValueSource(ints = {0, 1, 23})
    void acceptsEncodedBlobThroughImmutableEnvelopeBoundary(int envelopeBytes) throws Exception {
        try (FakeRedis redis = new FakeRedis()) {
            byte[] payload = new byte[64 * 1024 * 1024 + envelopeBytes];
            payload[0] = 42;
            payload[payload.length - 1] = 73;

            ZLinkBlobStored stored =
                    assertInstanceOf(
                            ZLinkBlobStored.class,
                            redis.store
                                    .put(
                                            new ZLinkBlobReference("chunk-a"),
                                            payload,
                                            Duration.ofSeconds(2),
                                            () -> false)
                                    .toCompletableFuture()
                                    .join());

            assertEquals(1, redis.commandAcquisitions);
            assertEquals(1, redis.puts);
            assertArrayEquals(payload, redis.payload);
            assertNotSame(payload, redis.payload);
            assertEquals(Instant.ofEpochMilli(1000), stored.storeNow());
            assertEquals(Instant.ofEpochMilli(3000), stored.expiresAt());
        }
    }

    @Test
    void rejectsOneByteBeyondEncodedBlobBoundBeforeRedisCommands() throws Exception {
        try (FakeRedis redis = new FakeRedis()) {
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            redis.store.put(
                                    new ZLinkBlobReference("chunk-a"),
                                    new byte[64 * 1024 * 1024 + 24],
                                    Duration.ofSeconds(2),
                                    () -> false));

            assertEquals(0, redis.commandAcquisitions);
            assertEquals(0, redis.puts);
        }
    }

    @Test
    void roundsFractionalMillisecondRetentionUpForStoredExpiry() throws Exception {
        try (FakeRedis redis = new FakeRedis(2)) {
            ZLinkBlobStored stored =
                    assertInstanceOf(
                            ZLinkBlobStored.class,
                            redis.store
                                    .put(
                                            new ZLinkBlobReference("chunk-a"),
                                            new byte[] {42},
                                            Duration.ofNanos(1_500_000),
                                            () -> false)
                                    .toCompletableFuture()
                                    .join());
            assertEquals(Instant.ofEpochMilli(1002), stored.expiresAt());
        }
    }

    private static final class FakeRedis implements AutoCloseable {
        final ZLinkRedisRelocationStore store =
                new ZLinkRedisRelocationStore(
                        new ZLinkRedisRelocationOptions()
                                .setConnectionString("redis://127.0.0.1:1")
                                .setKeyPrefix("blob-bound"));
        int commandAcquisitions;
        int puts;
        byte[] payload;
        final long expectedRetentionMs;

        FakeRedis() throws Exception {
            this(2000);
        }

        @SuppressWarnings("unchecked")
        FakeRedis(long expectedRetentionMs) throws Exception {
            this.expectedRetentionMs = expectedRetentionMs;
            RedisAsyncCommands<String, byte[]> commands =
                    (RedisAsyncCommands<String, byte[]>)
                            Proxy.newProxyInstance(
                                    RedisAsyncCommands.class.getClassLoader(),
                                    new Class<?>[] {RedisAsyncCommands.class},
                                    (proxy, method, arguments) -> {
                                        if (!method.getName().equals("eval")) {
                                            throw new AssertionError(
                                                    "unexpected Redis command: " + method);
                                        }
                                        puts++;
                                        assertEquals(ScriptOutputType.MULTI, arguments[1]);
                                        assertArrayEquals(
                                                new String[] {
                                                    "blob-bound:{zlink-relocation-v1}:blob:chunk-a"
                                                },
                                                (String[]) arguments[2]);
                                        byte[][] values = (byte[][]) arguments[3];
                                        payload = values[0];
                                        assertEquals(
                                                Long.toString(expectedRetentionMs),
                                                new String(values[1], StandardCharsets.UTF_8));
                                        var result =
                                                new AsyncCommand<String, byte[], List<Object>>(
                                                        new Command<>(
                                                                CommandType.EVAL,
                                                                new NestedMultiOutput<>(
                                                                        ZLinkRedisStringByteArrayCodec
                                                                                .INSTANCE)));
                                        result.complete(
                                                List.of("stored", 1000L, expectedRetentionMs));
                                        return result;
                                    });
            StatefulRedisConnection<String, byte[]> transport =
                    (StatefulRedisConnection<String, byte[]>)
                            Proxy.newProxyInstance(
                                    StatefulRedisConnection.class.getClassLoader(),
                                    new Class<?>[] {StatefulRedisConnection.class},
                                    (proxy, method, arguments) ->
                                            switch (method.getName()) {
                                                case "async" -> {
                                                    commandAcquisitions++;
                                                    yield commands;
                                                }
                                                case "closeAsync" ->
                                                        CompletableFuture.completedFuture(null);
                                                default ->
                                                        throw new AssertionError(
                                                                "unexpected Redis connection call: "
                                                                        + method);
                                            });
            Field owner = ZLinkRedisRelocationStore.class.getDeclaredField("connection");
            owner.setAccessible(true);
            Field connection = ZLinkRedisLocationConnection.class.getDeclaredField("connection");
            connection.setAccessible(true);
            connection.set(owner.get(store), CompletableFuture.completedFuture(transport));
        }

        @Override
        public void close() {
            store.close();
        }
    }
}
