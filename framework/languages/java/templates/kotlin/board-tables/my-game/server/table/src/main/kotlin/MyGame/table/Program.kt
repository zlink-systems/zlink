package MyGame.table

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
        options.addHandlersFromPackageOf<Program>()
        options
            .addRouteMesh(Names.MESH)
            .listen(config.meshEndpoint)
            .setRoutingIdPrefix(config.id)
            .objects()
            .server()
            .addEntrySpot<Entry>()
            .addSpotFactory<Table>(Names.TABLE) { preserveStateWith<Table, TableTransfer>() }
            .addActorFactory<Player, PlayerFactory>(Names.PLAYER) {
                preserveStateWith<Player, PlayerTransfer>()
            }
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
