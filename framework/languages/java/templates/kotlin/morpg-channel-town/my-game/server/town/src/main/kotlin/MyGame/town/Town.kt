package MyGame.town

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}
}

data class TownState(
    val players: MutableSet<String> = linkedSetOf(),
    val parties: MutableMap<String, MutableList<String>> = linkedMapOf(),
)

class Town(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val actors: ZLinkActorClient,
    private val spots: ZLinkSpotManager,
) : ZLinkSuspendingSpot<Player>() {
    var state = TownState()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<JoinTownRequest>()
        if (join.playerId != actorId || Names.townId(join.channel) != context.spotId())
            return ZLinkSpotActorJoinResult.reject()
        state.players.add(actorId)
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) = broadcast()

    override suspend fun onLeaveActorSuspending(actor: Player) {
        val id = actor.context.actorId()
        state.players.remove(id)
        val iterator = state.parties.values.iterator()
        while (iterator.hasNext()) {
            val members = iterator.next()
            members.remove(id)
            if (members.isEmpty()) iterator.remove()
        }
        broadcast()
    }

    // Disconnect retains channel and party membership until an explicit move.
    override suspend fun onDisconnectActorSuspending(actor: Player) {}

    private fun view() =
        TownView(
            context.spotId(),
            config.id,
            state.players.sorted(),
            state.parties.toSortedMap().map { (id, members) -> PartyView(id, members.toList()) },
        )

    suspend fun show(actor: Player) = actor.context.boundSession().kotlin().send(view()).await()

    private suspend fun broadcast() {
        val message = view()
        for (id in state.players) actors.kotlin().sendToActor(id, message).await()
    }

    suspend fun party(actor: Player, partyId: String, create: Boolean) {
        val id = actor.context.actorId()
        if (
            partyId.isBlank() ||
                id !in state.players ||
                state.parties.values.any { id in it } ||
                (if (create) partyId in state.parties
                else
                    state.parties[partyId]?.let { it.size >= Names.MAXIMUM_PARTY_MEMBERS } != false)
        ) {
            actor.context
                .boundSession()
                .kotlin()
                .send(ActionRejected(Rejection.INVALID_PARTY))
                .await()
            return
        }
        if (create) state.parties[partyId] = mutableListOf(id)
        else state.parties.getValue(partyId).add(id)
        broadcast()
    }

    suspend fun enter(actor: Player) {
        val party =
            state.parties.entries.firstOrNull { it.value.first() == actor.context.actorId() }
        if (party == null) {
            actor.context.boundSession().kotlin().send(ActionRejected(Rejection.NOT_LEADER)).await()
            return
        }
        val dungeonId = Names.dungeonId(party.key)
        val created =
            spots.kotlin().getOrCreate(dungeonId, Names.DUNGEON).inMesh(Names.MESH).await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Dungeon creation rejected" }
        val members = party.value.toList()
        val prepared =
            context
                .outbound()
                .kotlin()
                .requestToSpot<DungeonPrepared>(dungeonId, PrepareDungeon(party.key, members))
                .await()
        check(prepared.dungeonId == dungeonId) { "Dungeon preparation identity mismatch" }
        for (id in members) actors
            .kotlin()
            .sendToActor(id, JoinDungeon(dungeonId, party.key))
            .await()
    }
}

class TownTransfer : ZLinkSpotRelocationAdapter<Town> {
    override fun capture(
        spot: Town,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.state))
    }

    override fun restore(
        spot: Town,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.state = Json.mapper.readValue(state, TownState::class.java)
        return CompletableFuture.completedFuture(null)
    }
}
