package MyGame

import java.io.File
import org.springframework.context.annotation.Bean
import org.springframework.context.annotation.Configuration
import systems.zlink.framework.locations.redis.*

@Configuration(proxyBeanMethods = false)
class Stores {
    @Bean
    fun config(): NodeConfig =
        Json.mapper.readValue(File(System.getProperty("MyGame.config")), NodeConfig::class.java)

    @Bean(destroyMethod = "close")
    fun locations(config: NodeConfig) =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(requireNotNull(config.redisEndpoint))
                .setKeyPrefix(requireNotNull(config.keyPrefix) + "locations:")
        )

    @Bean(destroyMethod = "close")
    fun relocations(config: NodeConfig) =
        ZLinkRedisRelocationStore(
            ZLinkRedisRelocationOptions()
                .setConnectionString(requireNotNull(config.redisEndpoint))
                .setKeyPrefix(requireNotNull(config.keyPrefix) + "relocation:")
        )
}
