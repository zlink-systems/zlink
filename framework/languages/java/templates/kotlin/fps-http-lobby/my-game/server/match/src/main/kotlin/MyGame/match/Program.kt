package MyGame.match

import MyGame.*
import MyGame.match.infrastructure.zlink.*
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
        options.addHandlersFromPackageOf<Program>()
        val mesh =
            options
                .addRouteMesh(Names.MESH)
                .listen(config.meshEndpoint)
                .setRoutingIdPrefix(config.id)
        mesh
            .objects()
            .server()
            .addEntrySpot<Entry>()
            .addSpotFactory<Room>(Names.ROOM) {
                stableTypeLimit(config.roomCapacity)
                preserveStateWith<Room, RoomTransfer>()
            }
            .addActorFactory<Player, PlayerFactory>(Names.PLAYER) {
                preserveStateWith<Player, PlayerTransfer>()
            }
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
