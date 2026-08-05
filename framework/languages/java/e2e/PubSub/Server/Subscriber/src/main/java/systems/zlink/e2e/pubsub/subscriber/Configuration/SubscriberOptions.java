package systems.zlink.e2e.pubsub.subscriber.Configuration;

import java.util.HashSet;
import java.util.Set;
import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record SubscriberOptions(
    String rid,
    Set<String> topics,
    String httpEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDir,
    HandlerDelayOptions delay) {
    public SubscriberOptions {
        required(rid, "rid");
        required(httpEndpoint, "http-endpoint");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        required(logDir, "log-dir");
        Set<String> normalized = new HashSet<>(topics == null ? Set.of() : topics);
        normalized.removeIf(String::isBlank);
        normalized.add("all");
        topics = Set.copyOf(normalized);
        delay = delay == null ? new HandlerDelayOptions(0) : delay;
        if (delay.delayMillis() < 0) {
            throw new IllegalArgumentException("e2e.delay.delay-millis must be non-negative");
        }
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("e2e." + name + " is required");
        }
    }
}
