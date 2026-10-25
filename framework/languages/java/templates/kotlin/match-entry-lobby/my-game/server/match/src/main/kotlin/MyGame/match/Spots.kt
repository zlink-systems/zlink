package MyGame.match

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Matchmaking(
    override val context: ZLinkSpotContext,
    private val spots: ZLinkSpotManager,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    var state = MatchmakingState()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val queue = request.decode<QueueRequest>()
        require(queue.playerId == actorId) { "Queue identity mismatch" }
        state.enqueue(queue)
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) = match()

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    suspend fun queue(actor: Player, request: QueueRequest) {
        require(request.playerId == actor.context.actorId()) { "Queue identity mismatch" }
        state.enqueue(request)
        match()
    }

    private suspend fun match() {
        val pair = state.pair()
        if (pair.isEmpty()) return
        val roomId = state.allocateRoom()
        val created = spots.kotlin().getOrCreate(roomId, Names.ROOM).inMesh(Names.MESH).await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Room creation rejected" }
        state.remove(pair)
        for (player in pair) actors
            .kotlin()
            .sendToActor(player.playerId, RoomAssignment(roomId))
            .await()
    }
}

class Room(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    var state = RoomState()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val admission = request.decode<RoomAdmission>()
        require(admission.playerId == actorId && admission.roomId == context.spotId()) {
            "Room identity mismatch"
        }
        return if (state.admit(actorId)) ZLinkSpotActorJoinResult.accept()
        else ZLinkSpotActorJoinResult.reject()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        actors.kotlin().sendToActor(actor.context.actorId(), MatchFound(context.spotId())).await()
        broadcast(view())
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    private fun view() = RoomView(context.spotId(), config.id, state.phase, state.winner, state.view())

    suspend fun sendView(actor: Player) = actor.context.boundSession().kotlin().send(view()).await()

    private suspend fun broadcast(message: Any) {
        for (player in state.players) actors.kotlin().sendToActor(player.playerId, message).await()
    }

    suspend fun move(actor: Player, request: MoveRequest) {
        val rejection = state.move(actor.context.actorId(), request)
        if (rejection != null) {
            actor.context.boundSession().kotlin().send(rejection).await()
            return
        }
        val player = state.view().single { it.playerId == actor.context.actorId() }
        actor.context.boundSession().kotlin()
            .send(MoveResult(player.x, player.y, request.sequence)).await()
        broadcast(view())
    }

    suspend fun shoot(actor: Player, request: ShootRequest) {
        val rejection = state.shoot(actor.context.actorId(), request)
        if (rejection != null) {
            actor.context.boundSession().kotlin().send(rejection).await()
            return
        }
        actor.context.boundSession().kotlin()
            .send(ShotResult("hit", request.targetId, request.sequence)).await()
        broadcast(view())
        state.winner?.let { broadcast(MatchEnded(it)) }
    }
}

class MatchmakingTransfer : ZLinkSpotRelocationAdapter<Matchmaking> {
    override fun capture(
        spot: Matchmaking,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.state))
    }

    override fun restore(
        spot: Matchmaking,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.state = Json.mapper.readValue(state, MatchmakingState::class.java)
        return CompletableFuture.completedFuture(null)
    }
}

class RoomTransfer : ZLinkSpotRelocationAdapter<Room> {
    override fun capture(
        spot: Room,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(spot.state.capture())
    }

    override fun restore(
        spot: Room,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.state = RoomState().also { it.restore(state) }
        return CompletableFuture.completedFuture(null)
    }
}
