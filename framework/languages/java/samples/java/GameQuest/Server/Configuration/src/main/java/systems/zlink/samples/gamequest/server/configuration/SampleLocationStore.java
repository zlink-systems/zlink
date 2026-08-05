package systems.zlink.samples.gamequest.server.configuration;

import java.time.Duration;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;

public final class SampleLocationStore {
    private SampleLocationStore() {
    }

    public static ZLinkRedisLocationStore create(SampleTopology topology) {
        SampleTopology.Location location = topology.location();
        return new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions()
                .setConnectionString(location.redisEndpoint())
                .setKeyPrefix(location.redisKeyPrefix() + "locations:")
                .setCommandTimeout(Duration.ofMillis(500)));
    }
}
