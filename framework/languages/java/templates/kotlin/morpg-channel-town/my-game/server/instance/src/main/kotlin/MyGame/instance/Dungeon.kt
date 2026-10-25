package MyGame.instance

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Dungeon(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    var state = DungeonState()

    fun prepare(request: PrepareDungeon): DungeonPrepared =
        DungeonPrepared(context.spotId(), request.runId, state.prepare(context.spotId(), request))

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<JoinDungeon>()
        return if (state.accepts(actorId, context.spotId(), join))
            ZLinkSpotActorJoinResult.accept()
        else ZLinkSpotActorJoinResult.reject()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        if (state.join(actor.context.actorId())) {
            context.outbound().kotlin().sendToSpot(state.ownerSpotId, RunStarted(state.runId)).await()
        }
        broadcast()
    }

    private suspend fun broadcast() {
        val message = view()
        for (id in state.joined) actors.kotlin().sendToActor(id, message).await()
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    override suspend fun onDisconnectActorSuspending(actor: Player) {}

    private fun view() =
        DungeonView(
            context.spotId(),
            config.id,
            state.partyId,
            state.members,
            state.stage,
            state.runId,
            state.phase,
        )

    suspend fun show(actor: Player) = actor.context.boundSession().kotlin().send(view()).await()

    suspend fun showWithParty(actor: Player) {
        context
            .outbound()
            .kotlin()
            .sendToSpot(state.ownerSpotId, PartyViewRequest(actor.context.actorId()))
            .await()
        show(actor)
    }

    suspend fun advance(actor: Player) {
        val reason = state.advance()
        if (reason != null) {
            actor.context.boundSession().kotlin().send(ActionRejected(reason)).await()
            return
        }
        broadcast()
        if (state.phase == RunPhase.Completed) {
            context.outbound().kotlin().sendToSpot(state.ownerSpotId, RunCompleted(state.runId)).await()
        }
    }
}

class DungeonTransfer : ZLinkSpotRelocationAdapter<Dungeon> {
    override fun capture(
        spot: Dungeon,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.state))
    }

    override fun restore(
        spot: Dungeon,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.state = Json.mapper.readValue(state, DungeonState::class.java)
        return CompletableFuture.completedFuture(null)
    }
}
