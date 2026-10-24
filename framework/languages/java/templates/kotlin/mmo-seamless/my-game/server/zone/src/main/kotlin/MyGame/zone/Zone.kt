package MyGame.zone

import MyGame.*
import MyGame.border.BorderRoutes
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

// This projection supports visibility; Player owns authoritative movement state.
data class ZoneState(
    val visiblePlayers: MutableMap<String, EnterZone> = mutableMapOf(),
    val arrivals: MutableMap<String, EnterZone> = mutableMapOf(),
)

class Zone(override val context: ZLinkSpotContext, private val actors: ZLinkActorClient) :
    ZLinkSuspendingSpot<Player>() {
    var state = ZoneState()
    private val adjacent by lazy { Rules.adjacentZones(context.spotId()) }

    override fun configure() {
        adjacent.forEach { BorderRoutes.register(context.handlers(), it, context.spotId()) }
    }

    override suspend fun onInitializeSuspending() {
        context
            .addTimer<BorderTick>(
                Names.BORDER_TIMER,
                Duration.ofMillis(Rules.TICK_MILLIS),
                ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false),
            )
            .await()
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val arrival = request.decode<EnterZone>()
        if (Rules.zoneId(arrival.x) != context.spotId()) return ZLinkSpotActorJoinResult.reject()
        check(state.arrivals.putIfAbsent(actorId, arrival) == null) { "Duplicate Zone arrival" }
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        actor.apply(
            requireNotNull(state.arrivals.remove(actor.context.actorId())) {
                "Zone admission has no movement intent"
            }
        )
        remember(actor)
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {
        state.visiblePlayers.remove(actor.context.actorId())
    }

    override suspend fun onDisconnectActorSuspending(actor: Player) = onLeaveActorSuspending(actor)

    fun remember(actor: Player) {
        state.visiblePlayers[actor.context.actorId()] = actor.position
    }

    suspend fun publishBorder() {
        for ((actorId, position) in state.visiblePlayers) {
            val offset = position.x % Rules.ZONE_WIDTH
            if (offset >= Rules.BORDER_RADIUS && offset < Rules.ZONE_WIDTH - Rules.BORDER_RADIUS)
                continue
            val presence = BorderPresence(context.spotId(), actorId, position.x, position.sequence)
            for (target in adjacent) context
                .outbound()
                .kotlin()
                .publish(Names.BORDER, Rules.borderTopic(context.spotId(), target), presence)
                .await()
        }
    }

    suspend fun showBorder(presence: BorderPresence) {
        for ((observerId, observer) in state.visiblePlayers) {
            if (kotlin.math.abs(observer.x - presence.x) > Rules.BORDER_RADIUS) continue
            actors
                .kotlin()
                .sendToActor(
                    observerId,
                    BorderVisible(
                        context.spotId(),
                        presence.sourceZoneId,
                        presence.actorId,
                        presence.x,
                        presence.sequence,
                    ),
                )
                .await()
        }
    }
}

class ZoneTransfer : ZLinkSpotRelocationAdapter<Zone> {
    override fun capture(
        spot: Zone,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.state))
    }

    override fun restore(
        spot: Zone,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.state = Json.mapper.readValue(state, ZoneState::class.java)
        return CompletableFuture.completedFuture(null)
    }
}
