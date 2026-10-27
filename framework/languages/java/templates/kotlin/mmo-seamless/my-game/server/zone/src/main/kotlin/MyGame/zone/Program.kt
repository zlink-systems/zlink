package MyGame.zone

import MyGame.*
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.context.annotation.Bean
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode
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
        require(config.zoneCapacity > 0)
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
            .addSpotFactory<Zone>(Names.ZONE) {
                stableTypeLimit(config.zoneCapacity)
                executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
                preserveStateWith<Zone, ZoneTransfer>()
            }
            .addActorFactory<Player, PlayerFactory>(Names.PLAYER) {
                preserveStateWith<Player, PlayerTransfer>()
            }
        mesh.channelName(Names.BORDER).server()
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
