package MyGame.channel

import MyGame.*
import MyGame.channel.infrastructure.zlink.*
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
        require(config.zoneCapacity > 0) { "Zone capacity must be positive" }
        configure(options, locations, relocations)
        options.addHandlersFromPackageOf<Program>()
        options
            .addRouteMesh(Names.mesh(requireNotNull(config.worldId)))
            .listen(requireNotNull(config.meshEndpoint))
            .setRoutingIdPrefix(config.id)
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
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
