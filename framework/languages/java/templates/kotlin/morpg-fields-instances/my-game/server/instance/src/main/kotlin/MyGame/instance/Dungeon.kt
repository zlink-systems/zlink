package MyGame.instance

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

data class DungeonState(
    var partyId: String = "",
    var members: List<String> = emptyList(),
    var stage: Int = 0,
)

// User Spots retain Player Actor membership; Dungeon owns game progress.
class Dungeon(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    var state = DungeonState()

    fun prepare(request: PrepareDungeon): DungeonPrepared {
        val members = request.members
        require(
            state.partyId.isEmpty() &&
                request.partyId.isNotBlank() &&
                members.isNotEmpty() &&
                members.size <= Rules.MAXIMUM_PARTY_MEMBERS &&
                members.none { it.isBlank() } &&
                members.distinct().size == members.size &&
                Rules.dungeonId(request.partyId) == context.spotId()
        ) {
            "Invalid dungeon party preparation"
        }
        state.partyId = request.partyId
        state.members = members.toList()
        return DungeonPrepared(context.spotId())
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<JoinDungeon>()
        return if (
            join.dungeonId == context.spotId() &&
                join.partyId == state.partyId &&
                actorId in state.members
        )
            ZLinkSpotActorJoinResult.accept()
        else ZLinkSpotActorJoinResult.reject()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        actors.kotlin().sendToActor(actor.context.actorId(), view()).await()
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    override suspend fun onDisconnectActorSuspending(actor: Player) {}

    private fun view() =
        DungeonView(context.spotId(), config.id, state.partyId, state.members, state.stage)

    suspend fun show(actor: Player) {
        actor.context.boundSession().kotlin().send(view()).await()
    }

    suspend fun advance(requester: Player) {
        state.stage = Math.incrementExact(state.stage)
        val message = view()
        requester.context.boundSession().kotlin().send(message).await()
        for (id in state.members) if (id != requester.context.actorId())
            actors.kotlin().sendToActor(id, message).await()
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
