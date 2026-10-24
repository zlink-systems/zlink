package MyGame.instance

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.context.annotation.Bean
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.handlers.ZLinkSpotRequest
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.locations.redis.*
import systems.zlink.framework.spots.*
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
            .addSpotFactory<Dungeon>(Names.DUNGEON) { recreateOnRelocation() }
    }
}

// A game instance is a User Spot, not an actor-free Framework Instance Spot.
class Dungeon(private val context: ZLinkSpotContext) : ZLinkSpot<ZLinkActor> {
    override fun context() = context

    override fun onJoinedActor(actor: ZLinkActor): CompletionStage<Void> =
        CompletableFuture.completedFuture(null)

    override fun onLeaveActor(actor: ZLinkActor): CompletionStage<Void> =
        CompletableFuture.completedFuture(null)
}

class DungeonActions(private val config: NodeConfig) {
    @ZLinkSpotRequest
    suspend fun open(spot: Dungeon, request: OpenDungeonRequest): DungeonOpened {
        require(request.dungeonId == spot.context().spotId())
        return DungeonOpened(spot.context().spotId(), config.id)
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)
