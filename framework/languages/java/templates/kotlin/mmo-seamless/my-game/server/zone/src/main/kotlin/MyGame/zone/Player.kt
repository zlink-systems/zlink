package MyGame.zone

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.*
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class Player(override val context: ZLinkActorContext) : ZLinkSuspendingActor() {
    suspend fun deliver(message: Any, visible: Boolean = true) {
        val sourceSpotId: String? = when (message) {
            is PlayerAppeared -> message.sourceSpotId
            is PlayerMoved -> message.sourceSpotId
            is PlayerLeft -> message.sourceSpotId
            else -> null
        }
        if (sourceSpotId != null && (context.spotId().orElse(null) != sourceSpotId || !visible)) return
        context.boundSession().kotlin().send(message).await()
    }

    fun move(zoneId: String, destination: EnterZone) {
        context.joinSpot(zoneId, destination).defer()
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted ->
                context
                    .boundSession()
                    .kotlin()
                    .send(completion.reply().decode<WorldState>())
                    .await()
            is ZLinkActorJoinCompletion.Rejected ->
                context
                    .boundSession()
                    .kotlin()
                    .send(completion.reply().decode<ActionRejected>())
                    .await()
            is ZLinkActorJoinCompletion.Failed -> error("Zone join failed: ${completion.kind()}")
        }
    }
}

class PlayerFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor = Player(context)
}

class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    private val empty = byteArrayOf()

    override fun capture(
        actor: Player,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(empty)
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        require(state.isEmpty()) { "Player Actor has no application relocation state" }
        return CompletableFuture.completedFuture(null)
    }
}

class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}
}

@ZLinkHandlerGroup(Names.PLAYER)
class EnterWorld {
    @ZLinkSpotActorSend
    suspend fun join(
        entry: Entry,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinWorldRequest,
    ) {
        require(actor.context.actorId() == request.playerId)
        val location = ZonePositionDomain.resolveLocation(request.x)
        val rejection = location.rejection
        if (rejection != null) {
            actor.context.boundSession().kotlin().send(rejection).await()
            return
        }
        actor.move(
            requireNotNull(location.zoneId),
            EnterZone(Position(request.x), 0, 0),
        )
    }

    @ZLinkSpotActorSend
    suspend fun notificationPlayerAppeared(spot: Entry, actor: Player, context: ZLinkMessageContext, message: PlayerAppeared) {
        actor.deliver(message)
    }

    @ZLinkSpotActorSend
    suspend fun notificationPlayerMoved(spot: Entry, actor: Player, context: ZLinkMessageContext, message: PlayerMoved) {
        actor.deliver(message)
    }

    @ZLinkSpotActorSend
    suspend fun notificationPlayerLeft(spot: Entry, actor: Player, context: ZLinkMessageContext, message: PlayerLeft) {
        actor.deliver(message)
    }
}
