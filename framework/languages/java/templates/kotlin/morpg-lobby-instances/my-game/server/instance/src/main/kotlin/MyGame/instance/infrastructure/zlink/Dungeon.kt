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
        require(
            state.partyId.isEmpty() &&
                request.partyId.isNotBlank() &&
                request.members.isNotEmpty() &&
                request.members.size <= Rules.MAX_MEMBERS &&
                request.members.distinct().size == request.members.size &&
                request.members.none { it.isBlank() } &&
                Names.DUNGEON_PREFIX + request.partyId == context.spotId()
        ) {
            "Invalid dungeon party preparation"
        }
        state.partyId = request.partyId
        state.members = request.members.toList()
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

    private fun view() =
        DungeonView(context.spotId(), config.id, state.partyId, state.members, state.stage)

    suspend fun show(actor: Player) {
        actor.context.boundSession().kotlin().send(view()).await()
    }

    suspend fun advance() {
        state.stage = Math.incrementExact(state.stage)
        val view = view()
        for (id in state.members) actors.kotlin().sendToActor(id, view).await()
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
