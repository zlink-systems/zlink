package systems.zlink.samples.tictactoe.server.configuration;

import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;

import java.time.Duration;

public final class SampleLocationStore {
    private SampleLocationStore() {}

    public static ZLinkRedisLocationStore create(PlaySettings settings) {
        return create(settings.redisEndpoint(), settings.redisKeyPrefix());
    }

    public static ZLinkRedisLocationStore create(ApiSettings settings) {
        return create(settings.redisEndpoint(), settings.redisKeyPrefix());
    }

    private static ZLinkRedisLocationStore create(String redisEndpoint, String redisKeyPrefix) {
        return new ZLinkRedisLocationStore(
                new ZLinkRedisLocationOptions()
                        .setConnectionString(redisEndpoint)
                        .setKeyPrefix(redisKeyPrefix + "locations:")
                        .setCommandTimeout(Duration.ofMillis(500)));
    }
}
