package systems.zlink.framework.locations.redis;

import io.lettuce.core.RedisURI;
import java.time.Duration;
import java.util.Objects;

public final class ZLinkRedisRelocationOptions {
    private String connectionString;
    private String keyPrefix;
    private Duration commandTimeout;

    public String connectionString() {
        return connectionString;
    }

    public ZLinkRedisRelocationOptions setConnectionString(String value) {
        connectionString = requireText(value, "connectionString");
        return this;
    }

    public String keyPrefix() {
        return keyPrefix;
    }

    public ZLinkRedisRelocationOptions setKeyPrefix(String value) {
        keyPrefix = requireText(value, "keyPrefix");
        return this;
    }

    public Duration commandTimeout() {
        return commandTimeout;
    }

    public ZLinkRedisRelocationOptions setCommandTimeout(Duration value) {
        Objects.requireNonNull(value, "commandTimeout");
        if (value.isZero() || value.isNegative()) {
            throw new IllegalArgumentException("commandTimeout must be positive.");
        }
        commandTimeout = value;
        return this;
    }

    void validate() {
        requireText(connectionString, "connectionString");
        requireText(keyPrefix, "keyPrefix");
    }

    RedisURI redisUri() {
        String value = requireText(connectionString, "connectionString");
        RedisURI uri = RedisURI.create(value.contains("://") ? value : "redis://" + value);
        if (commandTimeout != null) {
            uri.setTimeout(commandTimeout);
        }
        return uri;
    }

    private static String requireText(String value, String name) {
        Objects.requireNonNull(value, name);
        if (value.isBlank()) {
            throw new IllegalArgumentException(name + " is required.");
        }
        return value;
    }
}
