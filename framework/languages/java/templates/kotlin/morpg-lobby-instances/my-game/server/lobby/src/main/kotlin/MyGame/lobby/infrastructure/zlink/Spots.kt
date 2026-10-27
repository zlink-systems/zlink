package MyGame.lobby.infrastructure.zlink

import MyGame.*
import MyGame.lobby.domain.PartyState
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Party(
    override val context: ZLinkSpotContext,
    private val actors: ZLinkActorClient,
    private val spots: ZLinkSpotManager,
) : ZLinkSuspendingSpot<Player>() {
    var state = PartyState()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<PartyJoin>()
        return if (
            join.partyId == context.spotId() && state.join(actorId, join)
        )
            ZLinkSpotActorJoinResult.accept()
        else ZLinkSpotActorJoinResult.reject()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) = broadcast()

    override suspend fun onLeaveActorSuspending(actor: Player) {
        state.leave(actor.context.actorId())
    }

    suspend fun show(actor: Player) {
        actor.context.boundSession().kotlin().send(state.view()).await()
    }

    suspend fun ready(actor: Player) {
        state.markReady(actor.context.actorId())
        broadcast()
    }

    private suspend fun broadcast() {
        val view = state.view()
        for (id in state.members) actors.kotlin().sendToActor(id, view).await()
    }

    suspend fun enter(actor: Player) {
        enterPlayer(actor.context.actorId())
    }
    suspend fun enterPlayer(playerId: String) {
        state.enter(playerId)?.let {
            actors.kotlin().sendToActor(playerId, ActionRejected(it)).await()
            return
        }
        if (state.phase == RunPhase.Active) { actors.kotlin().sendToActor(playerId, state.view()).await(); return }
        val runId = requireNotNull(state.runId)
        val id = Names.dungeonId(runId)
        val members = state.members.toList()
        val partyId = state.partyId
        val result = spots.kotlin().getOrCreate(id, Names.DUNGEON).inMesh(Names.MESH).await()
        check(result.state() != ZLinkSpotCreateState.REJECTED) { "Dungeon creation rejected" }
        val prepared =
            context
                .outbound()
                .kotlin()
                .requestToSpot<DungeonPrepared>(id, PrepareDungeon(runId, partyId, members))
                .await()
        check(prepared.dungeonId == id && state.prepared(prepared.runId) == null) { "Dungeon preparation identity mismatch" }
        val travel = JoinDungeon(id, runId)
        for (member in members.filter { it !in state.entered }) actors.kotlin().sendToActor(member, travel).await()
    }
}

class PartyTransfer : ZLinkSpotRelocationAdapter<Party> {
    override fun capture(
        spot: Party,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.state))
    }

    override fun restore(
        spot: Party,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.state = Json.mapper.readValue(state, PartyState::class.java)
        return CompletableFuture.completedFuture(null)
    }
}
