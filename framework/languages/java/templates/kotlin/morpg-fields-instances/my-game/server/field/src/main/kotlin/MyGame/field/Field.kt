package MyGame.field

import MyGame.*
import MyGame.border.BorderRoutes
import java.time.Duration
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

data class FieldState(val visiblePlayers: MutableMap<String, EnterField> = linkedMapOf())

class Field(
    override val context: ZLinkSpotContext,
    private val actors: ZLinkActorClient,
    private val spots: ZLinkSpotManager,
    private val config: NodeConfig,
) : ZLinkSuspendingSpot<Player>() {
    var state = FieldState()
    private val positions = FieldPositionDomain(context.spotId())
    private val adjacent = Rules.adjacentFields(context.spotId())
    private val borderTopics = adjacent.map { Rules.borderTopic(context.spotId(), it) }

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
        val rejection = positions.queueArrival(actorId, request.decode<EnterField>())
        return if (rejection == null) ZLinkSpotActorJoinResult.accept() else ZLinkSpotActorJoinResult.reject()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        val arrival = requireNotNull(positions.takeArrival(actor.context.actorId())) {
            "Field admission has no movement intent"
        }
        positions.addPlayer(actor.context.actorId(), arrival)
        actor.apply(arrival)
        remember(actor)
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {
        val id = actor.context.actorId()
        state.visiblePlayers.remove(id)
        positions.removePlayer(id)
    }

    override suspend fun onDisconnectActorSuspending(actor: Player) {
        val id = actor.context.actorId()
        state.visiblePlayers.remove(id)
        positions.setConnected(id, false)
    }

    fun remember(actor: Player) {
        positions.setConnected(actor.context.actorId(), true)
        state.visiblePlayers[actor.context.actorId()] = actor.position
    }

    suspend fun move(actor: Player, request: MoveRequest) {
        val result = positions.move(actor.context.actorId(), request)
        if (result.rejection != null) {
            actor.context.boundSession().kotlin().send(result.rejection).await()
            return
        }
        val arrival = EnterField(result.position.x, result.sequence, result.remainingMs)
        if (result.relocates) {
            actor.move(arrival)
            return
        }
        actor.apply(arrival)
        remember(actor)
        actor.sendState()
    }

    suspend fun publishBorder() {
        for ((actorId, position) in state.visiblePlayers) {
            val offset = position.x % Rules.FIELD_WIDTH
            if (offset >= Rules.BORDER_RADIUS && offset < Rules.FIELD_WIDTH - Rules.BORDER_RADIUS) continue
            val presence = BorderPresence(context.spotId(), actorId, position.x, position.sequence)
            for (topic in borderTopics)
                context.outbound().kotlin().publish(Names.BORDER, topic, presence).await()
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
                        presence.sourceFieldId,
                        presence.playerId,
                        presence.x,
                        presence.sequence,
                    ),
                )
                .await()
        }
    }

    suspend fun party(actor: Player, requestedPartyId: String?, create: Boolean) {
        if (actor.partyId != null) {
            reject(actor, Rejection.INVALID_PARTY)
            return
        }
        val partySpotId =
            if (create) "party-${UUID.randomUUID().toString().replace("-", "")}"
            else requestedPartyId ?: ""
        if (!PartyRun.validId(partySpotId, "party-")) {
            reject(actor, Rejection.INVALID_PARTY)
            return
        }
        if (create) {
            val created = spots.kotlin().getOrCreate(partySpotId, Names.PARTY).inMesh(Names.MESH).await()
            check(created.state() != ZLinkSpotCreateState.REJECTED) { "Party creation rejected" }
        } else if (!spots.find(partySpotId).await().isPresent) {
            reject(actor, Rejection.INVALID_PARTY)
            return
        }
        val result =
            context.outbound().kotlin().requestToSpot<PartyCommandResult>(
                partySpotId,
                PartyCommand(actor.context.actorId(), true, false),
            ).await()
        val rejection = result.rejection
        if (rejection != null) {
            reject(actor, rejection.reason)
            return
        }
        actor.partyId = partySpotId
    }

    suspend fun enter(actor: Player) {
        val partyId = actor.partyId
        if (partyId == null) {
            reject(actor, Rejection.NOT_LEADER)
            return
        }
        val result =
            context.outbound().kotlin().requestToSpot<PartyCommandResult>(
                partyId,
                PartyCommand(actor.context.actorId(), false, true),
            ).await()
        result.rejection?.let { reject(actor, it.reason) }
    }

    suspend fun show(actor: Player) {
        actor.sendState()
        val partyId = actor.partyId ?: return
        val result =
            context.outbound().kotlin().requestToSpot<PartyCommandResult>(
                partyId,
                PartyCommand(actor.context.actorId()),
            ).await()
        result.view?.let { actor.context.boundSession().kotlin().send(it).await() }
    }

    private suspend fun reject(actor: Player, reason: String) {
        actor.context.boundSession().kotlin().send(ActionRejected(reason)).await()
    }

    fun capture() = FieldSnapshot(LinkedHashMap(state.visiblePlayers), positions.capture())

    fun restore(snapshot: FieldSnapshot) {
        positions.restore(snapshot.positions)
        state.visiblePlayers.clear()
        state.visiblePlayers.putAll(snapshot.visiblePlayers)
    }
}

data class FieldSnapshot(
    val visiblePlayers: Map<String, EnterField>,
    val positions: FieldPositionSnapshot,
)

class FieldTransfer : ZLinkSpotRelocationAdapter<Field> {
    override fun capture(
        spot: Field,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.capture()))
    }

    override fun restore(
        spot: Field,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.restore(Json.mapper.readValue(state, FieldSnapshot::class.java))
        return CompletableFuture.completedFuture(null)
    }
}
