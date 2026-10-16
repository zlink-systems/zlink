package MyGame.instance.infrastructure.zlink

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.handlers.ZLinkSpotRequest
import systems.zlink.framework.spots.*

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
    suspend fun open(spot: Dungeon, request: DungeonInfoRequest): DungeonOpened {
        return DungeonOpened(spot.context().spotId(), config.id)
    }
}
