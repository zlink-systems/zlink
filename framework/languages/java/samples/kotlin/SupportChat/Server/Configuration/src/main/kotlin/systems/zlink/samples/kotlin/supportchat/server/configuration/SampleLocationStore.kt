package systems.zlink.samples.kotlin.supportchat.server.configuration

import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore

object SampleLocationStore {
    fun create(topology: SampleTopology): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(topology.location().redisEndpoint)
                .setKeyPrefix(topology.location().redisKeyPrefix),
        )
}
