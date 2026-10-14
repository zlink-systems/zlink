package MyGame.channel.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Zone(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    override suspend fun onActorJoinSuspending(
        id: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult =
        if (id == request.decode<EnterWorldRequest>().playerId) ZLinkSpotActorJoinResult.accept()
        else ZLinkSpotActorJoinResult.reject()

    private fun view(actor: Player) =
        PlayerView(
            requireNotNull(config.worldId),
            Names.channel(context.spotId()),
            config.id,
            actor.gold,
        )

    suspend fun show(actor: Player) {
        actor.context.boundSession().kotlin().send(view(actor)).await()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        actors.kotlin().sendToActor(actor.context.actorId(), view(actor)).await()
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}
}

class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}
}
