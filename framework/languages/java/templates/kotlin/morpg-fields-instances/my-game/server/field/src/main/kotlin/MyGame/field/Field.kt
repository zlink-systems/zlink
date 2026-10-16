package MyGame.field

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
data class FieldState(
    val visiblePlayers: MutableMap<String, EnterField> = mutableMapOf(),
    val parties: MutableMap<String, MutableList<String>> = mutableMapOf(),
    val arrivals: MutableMap<String, EnterField> = mutableMapOf(),
)

class Field(
    override val context: ZLinkSpotContext,
    private val actors: ZLinkActorClient,
    private val spots: ZLinkSpotManager,
    private val config: NodeConfig,
) : ZLinkSuspendingSpot<Player>() {
    var state = FieldState()
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
        val arrival = request.decode<EnterField>()
        if (Rules.fieldId(arrival.x) != context.spotId()) return ZLinkSpotActorJoinResult.reject()
        check(state.arrivals.putIfAbsent(actorId, arrival) == null) { "Duplicate Field arrival" }
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        actor.apply(
            requireNotNull(state.arrivals.remove(actor.context.actorId())) {
                "Field admission has no movement intent"
            }
        )
        remember(actor)
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {
        val id = actor.context.actorId()
        state.visiblePlayers.remove(id)
        val parties = state.parties.entries.iterator()
        while (parties.hasNext()) {
            val party = parties.next()
            if (party.value.remove(id)) {
                if (party.value.isEmpty()) parties.remove()
                break
            }
        }
    }

    override suspend fun onDisconnectActorSuspending(actor: Player) {
        state.visiblePlayers.remove(actor.context.actorId())
    }

    fun remember(actor: Player) {
        state.visiblePlayers[actor.context.actorId()] = actor.position
    }

    suspend fun publishBorder() {
        for ((actorId, position) in state.visiblePlayers) {
            val offset = position.x % Rules.FIELD_WIDTH
            if (offset >= Rules.BORDER_RADIUS && offset < Rules.FIELD_WIDTH - Rules.BORDER_RADIUS)
                continue
            val presence = BorderPresence(context.spotId(), actorId, position.x, position.sequence)
            for (topic in borderTopics) context
                .outbound()
                .kotlin()
                .publish(Names.BORDER, topic, presence)
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
                        presence.sourceFieldId,
                        presence.playerId,
                        presence.x,
                        presence.sequence,
                    ),
                )
                .await()
        }
    }

    private fun view() =
        FieldView(
            context.spotId(),
            config.id,
            state.visiblePlayers.keys.sorted(),
            state.parties.toSortedMap().map { (id, members) -> PartyView(id, members.toList()) },
        )

    private suspend fun broadcast(requester: Player) {
        val message = view()
        requester.context.boundSession().kotlin().send(message).await()
        for (id in state.visiblePlayers.keys) {
            if (id != requester.context.actorId()) actors.kotlin().sendToActor(id, message).await()
        }
    }

    suspend fun party(actor: Player, partyId: String, create: Boolean) {
        val id = actor.context.actorId()
        val members = state.parties[partyId]
        if (
            partyId.isBlank() ||
                id !in state.visiblePlayers ||
                state.parties.values.any { id in it } ||
                (if (create) members != null
                else members == null || members.size >= Rules.MAXIMUM_PARTY_MEMBERS)
        ) {
            actor.context
                .boundSession()
                .kotlin()
                .send(ActionRejected(Rejection.INVALID_PARTY))
                .await()
            return
        }
        if (create) state.parties[partyId] = mutableListOf(id) else requireNotNull(members).add(id)
        broadcast(actor)
    }

    suspend fun enter(actor: Player) {
        val party =
            state.parties.entries.firstOrNull { it.value.first() == actor.context.actorId() }
        if (party == null) {
            actor.context.boundSession().kotlin().send(ActionRejected(Rejection.NOT_LEADER)).await()
            return
        }
        val partyId = party.key
        val members = party.value.toList()
        val dungeonId = Rules.dungeonId(partyId)
        val created =
            spots.kotlin().getOrCreate(dungeonId, Names.DUNGEON).inMesh(Names.MESH).await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Dungeon creation rejected" }
        val prepared =
            context
                .outbound()
                .kotlin()
                .requestToSpot<DungeonPrepared>(dungeonId, PrepareDungeon(partyId, members))
                .await()
        check(prepared.dungeonId == dungeonId) { "Dungeon preparation identity mismatch" }
        for (id in members) actors.kotlin().sendToActor(id, JoinDungeon(dungeonId, partyId)).await()
    }
}

class FieldTransfer : ZLinkSpotRelocationAdapter<Field> {
    override fun capture(
        spot: Field,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.state))
    }

    override fun restore(
        spot: Field,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.state = Json.mapper.readValue(state, FieldState::class.java)
        return CompletableFuture.completedFuture(null)
    }
}
