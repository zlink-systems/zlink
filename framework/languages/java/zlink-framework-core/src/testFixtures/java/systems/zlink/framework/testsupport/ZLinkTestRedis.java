package systems.zlink.framework.testsupport;

import io.lettuce.core.ClientOptions;
import io.lettuce.core.RedisClient;
import io.lettuce.core.RedisURI;
import io.lettuce.core.SocketOptions;

import org.junit.jupiter.api.Assumptions;

import java.time.Duration;

/** Selects and probes the single Redis endpoint used by tests in this JVM. */
public final class ZLinkTestRedis {
    private ZLinkTestRedis() {}

    public static String endpoint() {
        Probe result = CachedProbe.RESULT;
        if (result.failure() != null) {
            String message = "Redis is unreachable at " + result.connectionString();
            if (result.required()) {
                throw new AssertionError(
                        "ZLINK_REDIS_TEST_ENDPOINT is set but " + message, result.failure());
            }
            Assumptions.abort(message + "; ZLINK_REDIS_TEST_ENDPOINT is not set");
        }
        return result.connectionString();
    }

    private static Probe probe() {
        String provided = System.getenv("ZLINK_REDIS_TEST_ENDPOINT");
        String configured = provided == null || provided.isBlank() ? null : provided;
        String connectionString = "redis://" + (configured == null ? "127.0.0.1:6379" : configured);
        // The same probe limit as the .NET fixture; it also covers the first client start.
        Duration timeout = Duration.ofSeconds(2);
        try {
            RedisURI uri = RedisURI.create(connectionString);
            uri.setTimeout(timeout);
            try (var client = RedisClient.create(uri)) {
                client.setOptions(
                        ClientOptions.builder()
                                .autoReconnect(false)
                                .socketOptions(
                                        SocketOptions.builder().connectTimeout(timeout).build())
                                .build());
                try (var connection = client.connect()) {
                    connection.sync().ping();
                }
            }
            return new Probe(connectionString, configured != null, null);
        } catch (RuntimeException failure) {
            return new Probe(connectionString, configured != null, failure);
        }
    }

    private record Probe(String connectionString, boolean required, RuntimeException failure) {}

    private static final class CachedProbe {
        private static final Probe RESULT = probe();
    }
}
