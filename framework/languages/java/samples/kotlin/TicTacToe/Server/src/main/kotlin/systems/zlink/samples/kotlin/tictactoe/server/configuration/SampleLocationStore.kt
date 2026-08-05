package systems.zlink.samples.kotlin.tictactoe.server.configuration

import java.time.Duration
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore

object SampleLocationStore {
    fun create(settings: SampleSettings): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(settings.redisEndpoint)
                .setKeyPrefix(settings.redisKeyPrefix + "locations:")
                .setCommandTimeout(Duration.ofMillis(500)),
        )
}
