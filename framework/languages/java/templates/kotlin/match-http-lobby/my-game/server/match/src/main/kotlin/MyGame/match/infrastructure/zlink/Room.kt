package MyGame.match.infrastructure.zlink

import MyGame.*
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
    val state = RoomState()
    val roster = RoomRoster()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        checkJoin(actorId, request.decode<JoinRoomRequest>())
        return if (roster.canAdmit(actorId) && state.admit(actorId)) ZLinkSpotActorJoinResult.accept()
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

    private fun view() =
        RoomView(context.spotId(), config.id, state.view(), state.phase, state.winner)

    fun prepare(request: PrepareRoom): RoomPrepared = roster.prepare(context.spotId(), request)

    suspend fun current(actor: Player) {
        reply(actor, view())
    }

    suspend fun move(actor: Player, request: MoveRequest) {
        val rejection = state.move(actor.context.actorId(), request)
        if (rejection != null) {
            reply(actor, rejection)
            return
        }
        val player = state.view().single { it.playerId == actor.context.actorId() }
        reply(actor, MoveResult(player.x, player.y, request.sequence))
        broadcast(view())
    }

    suspend fun shoot(actor: Player, request: ShootRequest) {
        val rejection = state.shoot(actor.context.actorId(), request)
        if (rejection != null) {
            reply(actor, rejection)
            return
        }
        reply(actor, ShotResult("hit", request.targetId, request.sequence))
        broadcast(view())
        state.winner?.let { broadcast(MatchEnded(it)) }
    }

    private suspend fun broadcast(message: Any) {
        for (playerId in state.view().map { it.playerId }) clients.kotlin().sendToActor(playerId, message).await()
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
            Json.mapper.writeValueAsBytes(
                RoomTransferSnapshot(Json.mapper.readTree(entity.state.capture()), entity.roster.capture())
            )
        )
    }

    override fun restore(
        entity: Room,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        val snapshot = Json.mapper.readValue(state, RoomTransferSnapshot::class.java)
        entity.state.restore(Json.mapper.writeValueAsBytes(snapshot.game))
        entity.roster.restore(snapshot.roster)
        return CompletableFuture.completedFuture(null)
    }
}

data class RoomTransferSnapshot(
    val game: com.fasterxml.jackson.databind.JsonNode,
    val roster: RoomRoster.RosterSnapshot,
)
