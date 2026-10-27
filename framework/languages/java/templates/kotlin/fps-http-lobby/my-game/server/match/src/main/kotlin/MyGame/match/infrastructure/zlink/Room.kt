package MyGame.match.infrastructure.zlink

import MyGame.*
import MyGame.match.domain.HttpRoomSnapshot
import MyGame.match.domain.RoomRoster
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
    var roster = RoomRoster()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        checkJoin(actorId, request.decode<JoinRoomRequest>())
        return if (roster.canAdmit(actorId) && state.admit(actorId)) {
            ZLinkSpotActorJoinResult.accept()
        } else {
            ZLinkSpotActorJoinResult.reject()
        }
    }

    fun checkJoin(id: String, request: JoinRoomRequest) {
        require(id == request.playerId && context.spotId() == request.roomId) {
            "Room admission identity mismatch"
        }
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        clients.kotlin().sendToActor(actor.context.actorId(), MatchFound(context.spotId())).await()
        broadcast(view())
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    fun prepare(request: PrepareRoom): RoomPrepared = roster.prepare(context.spotId(), request)

    private fun view() = RoomView(context.spotId(), config.id, state.view(), state.phase, state.winner)

    suspend fun current(actor: Player) {
        reply(actor, view())
    }

    suspend fun move(actor: Player, request: MoveRequest) {
        val rejected = state.move(actor.context.actorId(), request)
        if (rejected != null) {
            reply(actor, rejected)
            return
        }
        val player = state.players.single { it.playerId == actor.context.actorId() }
        reply(actor, MoveResult(player.position, request.sequence))
        broadcast(view())
    }

    suspend fun shoot(actor: Player, request: ShootRequest) {
        val decision = state.shoot(actor.context.actorId(), request)
        if (decision.rejected != null) {
            reply(actor, decision.rejected)
            return
        }
        reply(actor, requireNotNull(decision.result))
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
        return CompletableFuture.completedFuture(
            Json.mapper.writeValueAsBytes(HttpRoomSnapshot(entity.state.capture(), entity.roster.capture()))
        )
    }

    override fun restore(
        entity: Room,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        val snapshot = Json.mapper.readValue(state, HttpRoomSnapshot::class.java)
        entity.roster.restore(snapshot.roster)
        entity.state.restore(snapshot.game)
        return CompletableFuture.completedFuture(null)
    }
}
