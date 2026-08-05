package systems.zlink.samples.kotlin.deliverydispatch.server.configuration

import java.time.Duration
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore

object SampleLocationStore {
    fun create(): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(SampleTopology.RedisEndpoint)
                .setKeyPrefix(SampleTopology.RedisKeyPrefix + "locations:")
                .setCommandTimeout(Duration.ofMillis(500)),
        )
}
