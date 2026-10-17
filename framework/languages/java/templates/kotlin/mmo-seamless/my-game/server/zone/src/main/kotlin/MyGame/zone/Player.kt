package MyGame.zone

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.*
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class Player(override val context: ZLinkActorContext, private val config: NodeConfig) :
    ZLinkSuspendingActor() {
    var position = EnterZone()
        private set

    fun apply(position: EnterZone) {
        this.position = position
    }

    fun move(position: EnterZone) {
        context.joinSpot(Rules.zoneId(position.x), position).defer()
    }

    suspend fun sendState() {
        context
            .boundSession()
            .kotlin()
            .send(WorldState(Rules.zoneId(position.x), config.id, position.x, position.sequence))
            .await()
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        check(completion is ZLinkActorJoinCompletion.Accepted) { "Zone join failed: $completion" }
        sendState()
    }
}

class PlayerFactory(private val config: NodeConfig) : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        Player(context, config)
}

class PlayerTransfer : ZLinkActorRelocationAdapter<Player> {
    override fun capture(
        actor: Player,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(actor.position))
    }

    override fun restore(
        actor: Player,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        actor.apply(Json.mapper.readValue(state, EnterZone::class.java))
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
        actor.move(EnterZone(request.x, 0))
    }
}
