package systems.zlink.framework.locations.redis;

import io.lettuce.core.RedisURI;
import java.time.Duration;
import java.util.Objects;

public final class ZLinkRedisLocationOptions {
    private String connectionString;
    private String keyPrefix;
    private Duration commandTimeout;

    public String connectionString() {
        return connectionString;
    }

    public ZLinkRedisLocationOptions setConnectionString(String connectionString) {
        this.connectionString = requireText(connectionString, "connectionString");
        return this;
    }

    public String keyPrefix() {
        return keyPrefix;
    }

    public ZLinkRedisLocationOptions setKeyPrefix(String keyPrefix) {
        this.keyPrefix = requireKeyPrefix(keyPrefix);
        return this;
    }

    public Duration commandTimeout() {
        return commandTimeout;
    }

    public ZLinkRedisLocationOptions setCommandTimeout(Duration commandTimeout) {
        Objects.requireNonNull(commandTimeout, "commandTimeout");
        if (commandTimeout.isZero() || commandTimeout.isNegative()) {
            throw new IllegalArgumentException("commandTimeout must be positive.");
        }
        this.commandTimeout = commandTimeout;
        return this;
    }

    void validate() {
        requireText(connectionString, "connectionString");
        requireKeyPrefix(keyPrefix);
    }

    RedisURI redisUri() {
        String value = requireText(connectionString, "connectionString");
        if (value.contains("://")) {
            return applyTimeout(RedisURI.create(value));
        }
        return applyTimeout(RedisURI.create("redis://" + value));
    }

    private RedisURI applyTimeout(RedisURI uri) {
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

    private static String requireKeyPrefix(String value) {
        String prefix = requireText(value, "keyPrefix");
        if (prefix.indexOf('{') >= 0 || prefix.indexOf('}') >= 0) {
            throw new IllegalArgumentException(
                "keyPrefix must not contain Redis hash-tag braces.");
        }
        return prefix;
    }
}
