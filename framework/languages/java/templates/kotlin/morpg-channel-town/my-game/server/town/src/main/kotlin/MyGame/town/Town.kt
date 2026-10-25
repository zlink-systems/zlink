package MyGame.town

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

data class TownState(
    val players: MutableSet<String> = linkedSetOf(),
    val parties: MutableMap<String, PartyRun> = linkedMapOf(),
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
        if (
            join.playerId != actorId ||
                ChannelRules.validate(join.channel) != null ||
                Names.townId(join.channel) != context.spotId()
        ) return ZLinkSpotActorJoinResult.reject()
        state.players.add(actorId)
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) = broadcast()

    override suspend fun onLeaveActorSuspending(actor: Player) {
        val id = actor.context.actorId()
        state.players.remove(id)
        val iterator = state.parties.entries.iterator()
        while (iterator.hasNext()) {
            val party = iterator.next().value
            party.leave(id)
            if (party.members.isEmpty()) iterator.remove()
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
            state.parties.toSortedMap().values.map { it.view() },
        )

    suspend fun show(actor: Player) = actor.context.boundSession().kotlin().send(view()).await()

    suspend fun showParty(playerId: String) = actors.kotlin().sendToActor(playerId, view()).await()

    private suspend fun broadcast() {
        val message = view()
        for (id in state.players) actors.kotlin().sendToActor(id, message).await()
    }

    suspend fun party(actor: Player, partyId: String?, create: Boolean) {
        val id = actor.context.actorId()
        val existing = partyId?.let(state.parties::get)
        if (
            id !in state.players ||
                state.parties.values.any { id in it.members } ||
                (!create &&
                    (existing == null ||
                        existing.phase != RunPhase.Forming ||
                        existing.members.size >= Names.MAXIMUM_PARTY_MEMBERS))
        ) {
            actor.context.boundSession().kotlin().send(ActionRejected(Rejection.INVALID_PARTY)).await()
            return
        }
        if (create) {
            val party = PartyRun(members = mutableListOf(id))
            state.parties[party.partyId] = party
        } else {
            existing!!.members.add(id)
        }
        broadcast()
    }

    suspend fun enter(actor: Player) {
        val id = actor.context.actorId()
        val party = state.parties.values.firstOrNull { id in it.members }
        val reason = party?.enter(id) ?: if (party == null) Rejection.NOT_LEADER else null
        if (reason != null) {
            actor.context.boundSession().kotlin().send(ActionRejected(reason)).await()
            return
        }
        if (party!!.phase == RunPhase.Active) {
            show(actor)
            return
        }

        val runId = requireNotNull(party.runId)
        val dungeonId = Names.dungeonId(runId)
        val created = spots.kotlin().getOrCreate(dungeonId, Names.DUNGEON).inMesh(Names.MESH).await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Dungeon creation rejected" }
        val prepared =
            context
                .outbound()
                .kotlin()
                .requestToSpot<DungeonPrepared>(
                    dungeonId,
                    PrepareDungeon(runId, party.partyId, context.spotId(), party.members.toList()),
                )
                .await()
        if (
            prepared.reason != null ||
                prepared.dungeonId != dungeonId ||
                party.prepared(prepared.runId) != null
        ) {
            actor.context.boundSession().kotlin().send(ActionRejected(Rejection.RUN_MISMATCH)).await()
            return
        }
        val join = JoinDungeon(dungeonId, runId)
        for (member in party.members) {
            if (member in state.players) actors.kotlin().sendToActor(member, join).await()
        }
    }

    suspend fun changeChannel(actor: Player, channel: Long) {
        val party = state.parties.values.firstOrNull { actor.context.actorId() in it.members }
        val reason = ChannelRules.validate(channel) ?: party?.changeChannel()
        if (reason != null) {
            actor.context.boundSession().kotlin().send(ActionRejected(reason)).await()
            return
        }
        actor.context
            .joinSpot(
                Names.townId(channel),
                JoinTownRequest(actor.context.actorId(), channel),
            )
            .defer()
    }

    suspend fun runStarted(message: RunStarted) {
        val party = state.parties.values.singleOrNull { it.runId == message.runId }
        if (party == null || party.started(message.runId) != null) {
            rejectRunMismatch(party)
            return
        }
        broadcast()
    }

    suspend fun runCompleted(message: RunCompleted) {
        val party = state.parties.values.singleOrNull { it.runId == message.runId }
        if (party == null || party.completed(message.runId) != null) {
            rejectRunMismatch(party)
            return
        }
        broadcast()
    }

    private suspend fun rejectRunMismatch(party: PartyRun?) {
        val leader = party?.members?.firstOrNull() ?: return
        actors.kotlin().sendToActor(leader, ActionRejected(Rejection.RUN_MISMATCH)).await()
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
