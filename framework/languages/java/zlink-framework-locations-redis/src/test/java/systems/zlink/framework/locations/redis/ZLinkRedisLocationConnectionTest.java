package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;

import io.lettuce.core.RedisURI;
import io.lettuce.core.api.StatefulRedisConnection;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

final class ZLinkRedisLocationConnectionTest {
    @Test
    void completedConnectionCloseReentryUsesOneClosePublication() throws Exception {
        ZLinkRedisLocationConnection<byte[]> connection =
            ZLinkRedisLocationConnection.forBytes(
                RedisURI.create("redis://127.0.0.1:1"));
        AtomicInteger closes = new AtomicInteger();
        AtomicReference<CompletionStage<Void>> reentered = new AtomicReference<>();
        StatefulRedisConnection<String, byte[]> transport =
            (StatefulRedisConnection<String, byte[]>) Proxy.newProxyInstance(
                StatefulRedisConnection.class.getClassLoader(),
                new Class<?>[] {StatefulRedisConnection.class},
                (proxy, method, arguments) -> {
                    if (method.getName().equals("closeAsync")) {
                        closes.incrementAndGet();
                        reentered.compareAndSet(null, connection.closeAsync());
                        return CompletableFuture.completedFuture(null);
                    }
                    throw new UnsupportedOperationException(method.getName());
                });
        Field current = ZLinkRedisLocationConnection.class
            .getDeclaredField("connection");
        current.setAccessible(true);
        current.set(connection, CompletableFuture.completedFuture(transport));

        CompletionStage<Void> first = connection.closeAsync();
        first.toCompletableFuture().join();

        assertEquals(1, closes.get(),
            "an inline connection-close callback must not start another close");
        assertSame(first, reentered.get(),
            "the reentry must observe the exact close publication");
    }
}
