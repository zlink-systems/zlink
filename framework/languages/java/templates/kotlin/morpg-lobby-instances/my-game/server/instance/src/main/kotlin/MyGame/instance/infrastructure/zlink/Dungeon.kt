package MyGame.instance.infrastructure.zlink

import MyGame.*
import MyGame.instance.domain.DungeonState
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

    fun prepare(request: PrepareDungeon): DungeonPrepared {
        require(Names.dungeonId(request.runId) == context.spotId() && state.prepare(request) == null) { Rules.RUN_MISMATCH }
        return DungeonPrepared(context.spotId(), request.runId)
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        return if (state.admit(actorId, request.decode<JoinDungeon>(), context.spotId())) ZLinkSpotActorJoinResult.accept() else ZLinkSpotActorJoinResult.reject()
    }
    override suspend fun onJoinedActorSuspending(actor: Player) {
        val started = state.join(actor.context.actorId())
        context.outbound().kotlin().sendToSpot(state.partyId, DungeonMemberEntered(requireNotNull(state.runId), actor.context.actorId())).await()
        if (started) context.outbound().kotlin().sendToSpot(state.partyId, RunStarted(requireNotNull(state.runId))).await()
        broadcast()
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    private fun view() =
        DungeonView(context.spotId(), config.id, state.partyId, state.members, state.stage, state.runId, state.phase)

    suspend fun show(actor: Player) {
        actor.context.boundSession().kotlin().send(view()).await()
    }

    suspend fun enter(actor: Player) { context.outbound().kotlin().sendToSpot(state.partyId, EnterPartyRun(actor.context.actorId())).await() }
    suspend fun advance(actor: Player) {
        state.advance()?.let { actor.context.boundSession().kotlin().send(ActionRejected(it)).await(); return }
        if (state.phase == RunPhase.Completed) context.outbound().kotlin().sendToSpot(state.partyId, RunCompleted(requireNotNull(state.runId))).await()
        broadcast()
    }
    private suspend fun broadcast() {
        val view = view()
        for (id in state.joined) actors.kotlin().sendToActor(id, view).await()
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
