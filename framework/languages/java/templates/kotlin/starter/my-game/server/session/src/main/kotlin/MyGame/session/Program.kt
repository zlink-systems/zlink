package MyGame.session

import MyGame.*
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.context.annotation.Bean
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.locations.redis.*
import systems.zlink.framework.spring.*

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class Program {
    @Bean
    fun framework(
        config: NodeConfig,
        locations: ZLinkRedisLocationStore,
        relocations: ZLinkRedisRelocationStore,
    ) = ZLinkFrameworkConfigurer { options ->
        configure(options, locations, relocations)
        // The mesh listener is the starting point for adding nodes; no objects are registered.
        options.addRouteMesh(Names.MESH).listen(config.meshEndpoint).setRoutingIdPrefix(config.id)
        options
            .addStreamNode(Names.STREAM)
            .bind(config.streamEndpoint)
            .registerSession<PlayerSession>()
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
