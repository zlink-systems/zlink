package MyGame.zone

import MyGame.*
import MyGame.border.BorderRoutes
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

class Zone(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    val positions = ZonePositionDomain(context.spotId())
    private val adjacent by lazy { Rules.adjacentZones(context.spotId()) }

    override fun configure() {
        adjacent.forEach { BorderRoutes.register(context.handlers(), it, context.spotId()) }
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val arrival = request.decode<EnterZone>()
        val rejection = positions.queueArrival(actorId, arrival)
        if (rejection != null) return ZLinkSpotActorJoinResult.reject(ZLinkMessage.of(rejection))
        return ZLinkSpotActorJoinResult.accept(
            ZLinkMessage.of(
                WorldState(
                    context.spotId(),
                    config.id,
                    arrival.position.x,
                    arrival.sequence,
                )
            )
        )
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        val actorId = actor.context.actorId()
        val arrival = requireNotNull(positions.takeArrival(actorId)) {
            "Zone admission has no movement intent"
        }
        positions.addPlayer(actorId, arrival)
        publishLocal(actorId, PlayerAppeared(actorId, arrival.position))
        publishBorder(actorId, arrival.position, arrival.sequence, BorderPresenceKind.APPEARED)
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {
        val actorId = actor.context.actorId()
        val previous = positions.removePlayer(actorId) ?: return
        publishLeft(actorId, previous, PlayerLeftReason.LEFT_ZONE)
    }

    override suspend fun onDisconnectActorSuspending(actor: Player) {
        val actorId = actor.context.actorId()
        val state = positions.setConnected(actorId, false) ?: return
        publishLeft(actorId, state, PlayerLeftReason.DISCONNECTED)
    }

    suspend fun reconnect(actor: Player) {
        val actorId = actor.context.actorId()
        val previous = positions.tryState(actorId)
        if (previous == null) {
            actor.context.boundSession().kotlin().send(ActionRejected("outside-world")).await()
            return
        }
        val current = requireNotNull(positions.setConnected(actorId, true))
        if (!previous.connected) {
            publishLocal(actorId, PlayerAppeared(actorId, current.position))
            publishBorder(actorId, current.position, current.sequence, BorderPresenceKind.APPEARED)
        }
        sendWorldState(actor)
    }

    suspend fun move(actor: Player, request: MoveRequest) {
        val actorId = actor.context.actorId()
        val movement = positions.move(actorId, request)
        if (!movement.accepted) {
            actor.context.boundSession().kotlin().send(requireNotNull(movement.rejection)).await()
            return
        }
        if (movement.relocates) {
            actor.move(
                movement.zoneId,
                EnterZone(movement.position, movement.sequence, movement.remainingMs),
            )
            return
        }
        publishLocal(actorId, PlayerMoved(actorId, movement.position, movement.sequence))
        publishBorder(actorId, movement.position, movement.sequence, BorderPresenceKind.MOVED)
        sendWorldState(actor)
    }

    suspend fun showBorder(presence: BorderPresence) {
        for ((observerId, observer) in positions.connectedPlayers()) {
            if (
                observerId == presence.playerId ||
                    kotlin.math.abs(observer.position.x - presence.position.x) > Rules.BORDER_RADIUS
            )
                continue
            when (presence.kind) {
                BorderPresenceKind.APPEARED ->
                    actors
                        .kotlin()
                        .sendToActor(observerId, PlayerAppeared(presence.playerId, presence.position))
                        .await()
                BorderPresenceKind.MOVED ->
                    actors
                        .kotlin()
                        .sendToActor(
                            observerId,
                            PlayerMoved(presence.playerId, presence.position, presence.sequence),
                        )
                        .await()
                BorderPresenceKind.LEFT ->
                    actors
                        .kotlin()
                        .sendToActor(
                            observerId,
                            PlayerLeft(presence.playerId, presence.reason ?: PlayerLeftReason.LEFT_ZONE),
                        )
                        .await()
                else -> error("Unknown border event: ${presence.kind}")
            }
        }
    }

    fun capture() = positions.capture()

    fun restore(snapshot: ZonePositionSnapshot) = positions.restore(snapshot)

    private suspend inline fun <reified T : Any> publishLocal(playerId: String, event: T) {
        for (observerId in positions.connectedPlayers().keys) {
            if (observerId != playerId) actors.kotlin().sendToActor(observerId, event).await()
        }
    }

    private suspend fun publishLeft(playerId: String, state: PlayerStateView, reason: String) {
        publishLocal(playerId, PlayerLeft(playerId, reason))
        publishBorder(
            playerId,
            state.position,
            state.sequence,
            BorderPresenceKind.LEFT,
            reason,
        )
    }

    private suspend fun publishBorder(
        playerId: String,
        position: Position,
        sequence: Long,
        kind: String,
        reason: String? = null,
    ) {
        val offset = position.x % Rules.ZONE_WIDTH
        if (offset >= Rules.BORDER_RADIUS && offset < Rules.ZONE_WIDTH - Rules.BORDER_RADIUS) return
        for (target in adjacent) {
            context
                .outbound()
                .kotlin()
                .publish(
                    Names.BORDER,
                    Rules.borderTopic(context.spotId(), target),
                    BorderPresence(context.spotId(), playerId, position, sequence, kind, reason),
                )
                .await()
        }
    }

    private suspend fun sendWorldState(actor: Player) {
        val state = positions.state(actor.context.actorId())
        actor.context
            .boundSession()
            .kotlin()
            .send(WorldState(context.spotId(), config.id, state.position.x, state.sequence))
            .await()
    }
}

class ZoneTransfer : ZLinkSpotRelocationAdapter<Zone> {
    override fun capture(
        spot: Zone,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.capture()))
    }

    override fun restore(
        spot: Zone,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.restore(Json.mapper.readValue(state, ZonePositionSnapshot::class.java))
        return CompletableFuture.completedFuture(null)
    }
}
