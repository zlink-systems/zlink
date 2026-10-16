package MyGame.field

import MyGame.*
import MyGame.admission.*
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
        require(config.fieldCapacity > 0)
        configure(options, locations, relocations)
        options.addHandlersFromPackageOf<Program>()
        options.addHandlersFromPackageOf<EnterWorld>()
        val mesh =
            options
                .addRouteMesh(Names.MESH)
                .listen(config.meshEndpoint)
                .setRoutingIdPrefix(config.id)
        mesh
            .objects()
            .server()
            .addEntrySpot<Entry>()
            .addSpotFactory<Field>(Names.FIELD) {
                stableTypeLimit(config.fieldCapacity)
                executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
                preserveStateWith<Field, FieldTransfer>()
            }
            .addActorFactory<Player, PlayerFactory>(Names.PLAYER) {
                preserveStateWith<Player, PlayerTransfer>()
            }
        mesh.channelName(Names.BORDER).server()
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
