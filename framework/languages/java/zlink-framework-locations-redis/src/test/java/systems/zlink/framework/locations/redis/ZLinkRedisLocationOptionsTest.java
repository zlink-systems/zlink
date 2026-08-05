package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.time.Duration;
import org.junit.jupiter.api.Test;

final class ZLinkRedisLocationOptionsTest {
    @Test
    void acceptsDotnetStyleHostPortConnectionString() {
        ZLinkRedisLocationOptions options = new ZLinkRedisLocationOptions()
            .setConnectionString("127.0.0.1:16379")
            .setKeyPrefix("zlink:test");

        assertEquals("127.0.0.1", options.redisUri().getHost());
        assertEquals(16379, options.redisUri().getPort());
    }

    @Test
    void preservesExplicitRedisUriConnectionString() {
        ZLinkRedisLocationOptions options = new ZLinkRedisLocationOptions()
            .setConnectionString("redis://127.0.0.1:16379/0")
            .setKeyPrefix("zlink:test");

        assertEquals("127.0.0.1", options.redisUri().getHost());
        assertEquals(16379, options.redisUri().getPort());
        assertEquals(0, options.redisUri().getDatabase());
    }

    @Test
    void appliesCommandTimeoutToRedisUri() {
        ZLinkRedisLocationOptions options = new ZLinkRedisLocationOptions()
            .setConnectionString("127.0.0.1:16379")
            .setKeyPrefix("zlink:test")
            .setCommandTimeout(Duration.ofMillis(500));

        assertEquals(Duration.ofMillis(500), options.redisUri().getTimeout());
    }
}
