package MyGame.session

import MyGame.*
import MyGame.session.infrastructure.zlink.GameSession
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
        options
            .addRouteMesh(Names.MESH)
            .listen(config.meshEndpoint)
            .setRoutingIdPrefix(config.id)
            .objects()
            .client()
        options
            .addStreamNode(Names.STREAM)
            .bind(requireNotNull(config.streamEndpoint))
            .enableActorDispatch()
            .registerSession<GameSession>()
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
