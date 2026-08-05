package systems.zlink.samples.kotlin.bingo.server.configuration

import java.time.Duration
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore

object SampleLocationStore {
    fun create(topology: SampleTopology): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(topology.redisEndpoint)
                .setKeyPrefix("${topology.redisKeyPrefix}locations:")
                .setCommandTimeout(Duration.ofMillis(500)),
        )
}
