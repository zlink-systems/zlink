package MyGame.region.infrastructure.zlink

import MyGame.*
import MyGame.region.domain.*
import com.fasterxml.jackson.core.type.TypeReference
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Region(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    val positions = Positions()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<JoinRegion>()
        require(context.spotId() == Names.regionId(join.region, join.index)) {
            "Region join identity mismatch"
        }
        if (!positions.admit(actorId, config.capacity))
            return ZLinkSpotActorJoinResult.reject(
                ZLinkMessage.of(RegionFull(Names.REGION_FULL, join.region, join.index))
            )
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        broadcast(view())
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {
        positions.leave(actor.context.actorId())
        broadcast(view())
    }

    // Session disconnect retains Region membership until explicit leave.
    override suspend fun onDisconnectActorSuspending(actor: Player) {}

    private fun view() = RegionView(context.spotId(), config.id, positions.view())

    suspend fun show(actor: Player) {
        actor.context.boundSession().kotlin().send(view()).await()
    }

    suspend fun move(actor: Player, request: MoveRequest) {
        val id = actor.context.actorId()
        val rejection = positions.move(id, request.delta, request.sequence)
        if (rejection != null) {
            actor.context.boundSession().kotlin().send(rejection).await()
            return
        }
        val view = view()
        actor.context.boundSession().kotlin().send(view).await()
        broadcast(view, id)
    }

    private suspend fun broadcast(view: RegionView, excluded: String? = null) {
        for (player in view.players) if (player.playerId != excluded)
            actors.kotlin().sendToActor(player.playerId, view).await()
    }
}

class RegionTransfer : ZLinkSpotRelocationAdapter<Region> {
    private val stateType = object : TypeReference<LinkedHashMap<String, Position>>() {}

    override fun capture(
        spot: Region,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(
            Json.mapper.writeValueAsBytes(spot.positions.snapshot())
        )
    }

    override fun restore(
        spot: Region,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.positions.restore(Json.mapper.readValue(state, stateType))
        return CompletableFuture.completedFuture(null)
    }
}
