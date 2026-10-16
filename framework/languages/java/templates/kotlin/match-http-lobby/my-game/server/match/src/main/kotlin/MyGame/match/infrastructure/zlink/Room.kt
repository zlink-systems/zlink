package MyGame.match.infrastructure.zlink

import MyGame.*
import MyGame.match.domain.RoomState
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Room(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val clients: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    var state = RoomState()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        checkJoin(actorId, request.decode<JoinRoomRequest>())
        return if (state.admit(actorId)) ZLinkSpotActorJoinResult.accept()
        else ZLinkSpotActorJoinResult.reject()
    }

    fun checkJoin(id: String, request: JoinRoomRequest) {
        require(id == request.playerId && context.spotId() == request.roomId) {
            "Room admission identity mismatch"
        }
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        broadcast(view())
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    private fun view() = RoomView(context.spotId(), config.id, state.view())

    suspend fun current(actor: Player) {
        reply(actor, view())
    }

    suspend fun move(actor: Player, request: MoveRequest) {
        val reason = state.move(actor.context.actorId(), request)
        if (reason == null) broadcast(view()) else reply(actor, ActionRejected(reason))
    }

    suspend fun shoot(actor: Player, request: ShootRequest) {
        val reason = state.shoot(actor.context.actorId(), request)
        if (reason != null) {
            reply(actor, ShotRejected(reason))
            return
        }
        broadcast(view())
        state.winner?.let { broadcast(MatchEnded(it)) }
    }

    private suspend fun broadcast(message: Any) {
        for (player in state.players) clients.kotlin().sendToActor(player.playerId, message).await()
    }

    companion object {
        suspend fun reply(actor: Player, message: Any) {
            actor.context.boundSession().kotlin().send(message).await()
        }
    }
}

class RoomTransfer : ZLinkSpotRelocationAdapter<Room> {
    override fun capture(
        entity: Room,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(entity.state))
    }

    override fun restore(
        entity: Room,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        entity.state = Json.mapper.readValue(state, RoomState::class.java)
        return CompletableFuture.completedFuture(null)
    }
}
