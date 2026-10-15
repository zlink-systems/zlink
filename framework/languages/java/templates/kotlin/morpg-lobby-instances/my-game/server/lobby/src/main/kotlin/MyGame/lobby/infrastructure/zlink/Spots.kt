package MyGame.lobby.infrastructure.zlink

import MyGame.*
import MyGame.lobby.domain.PartyState
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Entry(override val context: ZLinkEntrySpotContext, private val spots: ZLinkSpotManager) :
    ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    suspend fun join(actor: Player, partyId: String, create: Boolean) {
        if (partyId.isBlank()) {
            actor.context.boundSession().kotlin().send(ActionRejected(Rules.INVALID_PARTY)).await()
            return
        }
        val id = Names.PARTY_PREFIX + partyId
        val result = spots.kotlin().getOrCreate(id, Names.PARTY).inMesh(Names.MESH).await()
        check(result.state() != ZLinkSpotCreateState.REJECTED) { "Party creation rejected" }
        actor.context.joinSpot(id, PartyJoin(partyId, create)).defer()
    }
}

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
            Names.PARTY_PREFIX + join.partyId == context.spotId() && state.join(actorId, join)
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
        state.ready.add(actor.context.actorId())
        broadcast()
    }

    private suspend fun broadcast() {
        val view = state.view()
        for (id in state.members) actors.kotlin().sendToActor(id, view).await()
    }

    suspend fun enter(actor: Player) {
        state.entryRejection(actor.context.actorId())?.let {
            actor.context.boundSession().kotlin().send(ActionRejected(it)).await()
            return
        }
        val id = Names.DUNGEON_PREFIX + state.partyId
        val members = state.members.toList()
        val partyId = state.partyId
        val result = spots.kotlin().getOrCreate(id, Names.DUNGEON).inMesh(Names.MESH).await()
        check(result.state() != ZLinkSpotCreateState.REJECTED) { "Dungeon creation rejected" }
        val prepared =
            context
                .outbound()
                .kotlin()
                .requestToSpot<DungeonPrepared>(id, PrepareDungeon(partyId, members))
                .await()
        check(prepared.dungeonId == id) { "Dungeon preparation identity mismatch" }
        val travel = JoinDungeon(id, partyId)
        for (member in members) actors.kotlin().sendToActor(member, travel).await()
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
