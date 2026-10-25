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
    private val playerIds = sortedSetOf<String>()

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
        playerIds.add(actor.context.actorId())
        actors.kotlin().sendToActor(actor.context.actorId(), view(actor)).await()
        publishPlayers()
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {
        if (playerIds.remove(actor.context.actorId())) publishPlayers()
    }

    override suspend fun onDisconnectActorSuspending(actor: Player) {
        onLeaveActorSuspending(actor)
    }

    private suspend fun publishPlayers() {
        val view = ChannelView(Names.channel(context.spotId()), playerIds.toList())
        for (playerId in playerIds) {
            actors.kotlin().sendToActor(playerId, view).await()
        }
    }

    suspend fun showChannelView(actor: Player) {
        actor.context
            .boundSession()
            .kotlin()
            .send(ChannelView(Names.channel(context.spotId()), playerIds.toList()))
            .await()
    }

    fun snapshot(): List<String> = playerIds.toList()

    fun restore(ids: List<String>) {
        playerIds.clear()
        playerIds.addAll(ids)
    }
}

class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}
}
