package MyGame.api

import MyGame.*
import io.lettuce.core.RedisClient
import java.net.URI
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.context.annotation.Bean
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.locations.redis.*
import systems.zlink.framework.spring.*

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class Program {
    @Bean(destroyMethod = "shutdown")
    fun redis(config: NodeConfig): RedisClient {
        val endpoint = config.redisEndpoint
        return RedisClient.create(if ("://" in endpoint) endpoint else "redis://$endpoint")
    }

    @Bean
    fun framework(
        config: NodeConfig,
        locations: ZLinkRedisLocationStore,
        relocations: ZLinkRedisRelocationStore,
    ) = ZLinkFrameworkConfigurer { options ->
        configure(options, locations, relocations)
        options.addHandlersFromPackageOf<Program>()
        val endpoint = URI.create(requireNotNull(config.apiEndpoint))
        options
            .addClientServerChannel(Names.PLAYER_DATA)
            .server()
            .setBindHost(endpoint.host)
            .listen(endpoint.port)
            .addHandlerGroup(Names.PLAYER_DATA)
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
