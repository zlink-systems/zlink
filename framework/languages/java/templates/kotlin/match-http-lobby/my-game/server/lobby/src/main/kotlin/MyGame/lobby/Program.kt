package MyGame.lobby

import MyGame.*
import MyGame.lobby.application.*
import MyGame.lobby.infrastructure.*
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.context.annotation.Bean
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.locations.redis.*
import systems.zlink.framework.spots.ZLinkSpotManager
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
    }

    @Bean(destroyMethod = "close")
    fun http(config: NodeConfig, spots: ZLinkSpotManager, routes: ZLinkRouteClient): HttpLobby {
        require(!config.sessionEndpoint.isNullOrBlank()) { "Session endpoint is required" }
        return HttpLobby(config, Matchmaker(ZLinkRooms(spots, routes, config)))
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
