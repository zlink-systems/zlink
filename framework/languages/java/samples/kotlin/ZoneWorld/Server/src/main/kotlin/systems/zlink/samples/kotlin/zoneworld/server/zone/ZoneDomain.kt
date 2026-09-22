package systems.zlink.samples.kotlin.zoneworld.server.zone

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.registerKotlinModule
import java.time.Duration
import java.time.Instant
import java.util.HexFormat
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.awaitCancellation
import org.springframework.beans.factory.ObjectProvider
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorFactory
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorJoinOperationId
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter
import systems.zlink.framework.actors.ZLinkRelocationCancellation
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.handlers.ZLinkSpotActorSend
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.kotlin.ZLinkSuspendingActor
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingPublishHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorSendHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotPacketHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotSubscriptionHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.decode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.sendToSpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.framework.spots.ZLinkEntrySpot
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkTimer
import systems.zlink.framework.spots.ZLinkTimerTick
import systems.zlink.samples.kotlin.zoneworld.dynamic.BorderSubscriptionHandlers
import systems.zlink.samples.kotlin.zoneworld.server.configuration.MaintenanceStore
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeCensus
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeMaintenanceState
import systems.zlink.samples.kotlin.zoneworld.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec

class PlayerActor(val actorId: String, override val context: ZLinkActorContext) :
    ZLinkSuspendingActor() {
    var x: Int = 0
        private set

    var y: Int = 0
        private set

    var zoneId: String = ""
        private set

    var isBot: Boolean = false
        private set

    var dirX: Int = 0
        private set

    var dirY: Int = 0
        private set

    private var pendingX = 0
    private var pendingY = 0
    private var pendingZone = ""
    private var pendingJoin = false
    private var pendingPurpose = JoinPurpose.NONE
    private val completedJoins = linkedSetOf<ZLinkActorJoinOperationId>()

    private enum class JoinPurpose {
        NONE,
        INITIAL_HUMAN,
        INITIAL_BOT,
        ZONE_CHANGE,
        CRASH_PROBE,
    }

    val pending: Boolean
        get() = pendingJoin

    val pendingTargetX: Int
        get() = pendingX

    val pendingTargetY: Int
        get() = pendingY

    val pendingTargetZone: String
        get() = pendingZone

    val pendingPurposeName: String
        get() = pendingPurpose.name

    val completedJoinIds: List<ZLinkActorJoinOperationId>
        get() = completedJoins.toList()

    fun prepareEntry(x: Int, y: Int, bot: Boolean, dirX: Int, dirY: Int) {
        this.x = x
        this.y = y
        this.isBot = bot
        this.dirX = dirX
        this.dirY = dirY
        pendingX = x
        pendingY = y
        pendingZone = ZoneWorldSpec.zoneOf(x, y)
        pendingJoin = true
        pendingPurpose = if (bot) JoinPurpose.INITIAL_BOT else JoinPurpose.INITIAL_HUMAN
    }

    fun prepareMove(x: Int, y: Int, zone: String) {
        pendingX = x
        pendingY = y
        pendingZone = zone
        pendingJoin = true
        pendingPurpose = JoinPurpose.ZONE_CHANGE
    }

    fun prepareCrashProbe(x: Int, y: Int, zone: String) {
        prepareMove(x, y, zone)
        pendingPurpose = JoinPurpose.CRASH_PROBE
    }

    fun applyAtZone(x: Int, y: Int, zone: String, bot: Boolean) {
        this.x = x
        this.y = y
        zoneId = zone
        isBot = bot
        pendingJoin = false
    }

    fun restoreState(
        x: Int,
        y: Int,
        zone: String,
        bot: Boolean,
        dirX: Int,
        dirY: Int,
        pendingX: Int,
        pendingY: Int,
        pendingZone: String,
        pendingJoin: Boolean,
        pendingPurpose: String,
        completedJoins: List<ZLinkActorJoinOperationId>,
    ) {
        this.x = x
        this.y = y
        zoneId = zone
        isBot = bot
        this.dirX = dirX
        this.dirY = dirY
        this.pendingX = pendingX
        this.pendingY = pendingY
        this.pendingZone = pendingZone
        this.pendingJoin = pendingJoin
        this.pendingPurpose = JoinPurpose.valueOf(pendingPurpose)
        this.completedJoins.clear()
        completedJoins.forEach(::rememberJoin)
    }

    fun updatePosition(x: Int, y: Int) {
        this.x = x
        this.y = y
    }

    fun reverseDirection() {
        dirX = -dirX
        dirY = -dirY
    }

    suspend fun send(message: Any) {
        if (!isBot) context.boundSession().kotlin().send(message).await()
    }

    // --8<-- [start:doc-zw-join-completed]
    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        val operationId =
            when (completion) {
                is ZLinkActorJoinCompletion.Accepted -> completion.operationId()
                is ZLinkActorJoinCompletion.Rejected -> completion.operationId()
                is ZLinkActorJoinCompletion.Failed -> completion.operationId()
            }
        if (operationId in completedJoins) return
        rememberJoin(operationId)
        pendingJoin = false
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> {
                val reply = completion.reply().decode<Messages.EnterZoneRes>()
                val joinedZone =
                    reply.zoneId.ifBlank {
                        pendingZone.ifBlank { ZoneWorldSpec.zoneOf(pendingX, pendingY) }
                    }
                applyAtZone(pendingX, pendingY, joinedZone, isBot)
                pendingZone = ""
                when (pendingPurpose) {
                    JoinPurpose.INITIAL_HUMAN ->
                        send(Messages.JoinWorldNotify(actorId, joinedZone, x, y))
                    JoinPurpose.CRASH_PROBE -> send(Messages.CrashRelocationProbeRes())
                    else -> Unit
                }
                pendingPurpose = JoinPurpose.NONE
            }
            else -> {
                val reason =
                    when (completion) {
                        is ZLinkActorJoinCompletion.Rejected ->
                            completion.reply().decode<Messages.EnterZoneRes>().error ?: "Rejected"
                        is ZLinkActorJoinCompletion.Failed -> mapFailure(completion.kind().name)
                        else -> error("unreachable")
                    }
                pendingZone = ""
                when (pendingPurpose) {
                    JoinPurpose.INITIAL_HUMAN ->
                        send(
                            Messages.JoinWorldNotify(
                                actorId,
                                ZoneWorldSpec.zoneOf(pendingX, pendingY),
                                pendingX,
                                pendingY,
                                reason,
                            )
                        )
                    JoinPurpose.CRASH_PROBE -> send(Messages.CrashRelocationProbeRes(reason))
                    else ->
                        if (!isBot) send(Messages.MoveRejectedNotify(reason, x, y))
                        else reverseDirection()
                }
                pendingPurpose = JoinPurpose.NONE
            }
        }
    }

    // --8<-- [end:doc-zw-join-completed]

    private fun mapFailure(kind: String) =
        when (kind) {
            "UNAVAILABLE" -> "Unavailable"
            "DEADLINE_EXCEEDED" -> "DeadlineExceeded"
            "SHUTTING_DOWN" -> "ShuttingDown"
            "NOT_FOUND" -> "NotFound"
            "REJECTED" -> "Rejected"
            else -> "InternalFailure"
        }

    private fun rememberJoin(operationId: ZLinkActorJoinOperationId) {
        if (!completedJoins.add(operationId)) return
        while (completedJoins.size > 256) completedJoins.remove(completedJoins.first())
    }
}

class PlayerActorFactory : ZLinkActorFactory {
    override fun create(context: ZLinkActorContext): CompletionStage<ZLinkActor> =
        CompletableFuture.completedFuture(PlayerActor(context.actorId(), context))
}

class PlayerActorRelocationAdapter : ZLinkActorRelocationAdapter<PlayerActor> {
    private val mapper = ObjectMapper().registerKotlinModule()

    private data class State(
        val x: Int,
        val y: Int,
        val zone: String,
        val bot: Boolean,
        val dirX: Int,
        val dirY: Int,
        val pendingX: Int,
        val pendingY: Int,
        val pendingZone: String,
        val pendingJoin: Boolean,
        val pendingPurpose: String,
        val completedJoins: List<OperationId>,
    )

    private data class OperationId(val high: Long, val low: Long)

    // --8<-- [start:doc-zw-actor-capture]
    override fun capture(
        actor: PlayerActor,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> =
        CompletableFuture.completedFuture(
            mapper.writeValueAsBytes(
                State(
                    actor.x,
                    actor.y,
                    actor.zoneId,
                    actor.isBot,
                    actor.dirX,
                    actor.dirY,
                    actor.pendingTargetX,
                    actor.pendingTargetY,
                    actor.pendingTargetZone,
                    actor.pending,
                    actor.pendingPurposeName,
                    actor.completedJoinIds.map { OperationId(it.high(), it.low()) },
                )
            )
        )

    // --8<-- [end:doc-zw-actor-capture]
    override fun restore(
        actor: PlayerActor,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        val value = mapper.readValue(state, State::class.java)
        actor.restoreState(
            value.x,
            value.y,
            value.zone,
            value.bot,
            value.dirX,
            value.dirY,
            value.pendingX,
            value.pendingY,
            value.pendingZone,
            value.pendingJoin,
            value.pendingPurpose,
            value.completedJoins.map { ZLinkActorJoinOperationId(it.high, it.low) },
        )
        return CompletableFuture.completedFuture(null)
    }
}

class ZoneEntrySpot(private val context: ZLinkEntrySpotContext) : ZLinkEntrySpot<PlayerActor> {
    override fun context(): ZLinkEntrySpotContext = context

    override fun onCreateActor(
        actor: PlayerActor,
        createRequest: ZLinkMessage,
    ): CompletionStage<ZLinkActorCreateResponse> {
        if (createRequest.isEmpty)
            return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept())
        val request = createRequest.decode<Messages.EnterWorldReq>()
        if (!ZoneWorldSpec.inRange(request.x, request.y)) {
            return CompletableFuture.completedFuture(
                ZLinkActorCreateResponse.reject(
                    Messages.EnterWorldRes("", request.x, request.y, "OutOfRange")
                )
            )
        }
        val zone = ZoneWorldSpec.zoneOf(request.x, request.y)
        actor.prepareEntry(request.x, request.y, request.isBot, request.dirX, request.dirY)
        if (request.isBot) {
            actor
                .context()
                .joinSpot(
                    zone,
                    Messages.EnterZoneReq(
                        actor.actorId,
                        request.x,
                        request.y,
                        true,
                        true,
                        "",
                        false,
                    ),
                )
                .timeout(Duration.ofSeconds(10))
                .defer()
        }
        return CompletableFuture.completedFuture(
            ZLinkActorCreateResponse.accept(
                Messages.JoinWorldNotify(actor.actorId, zone, request.x, request.y)
            )
        )
    }

    override fun onJoinedActor(actor: PlayerActor): CompletionStage<Void> =
        CompletableFuture.completedFuture<Void>(null)

    override fun onLeaveActor(actor: PlayerActor): CompletionStage<Void> =
        CompletableFuture.completedFuture<Void>(null)
}

class ZoneSpot(
    override val context: ZLinkSpotContext,
    private val maintenance: NodeMaintenanceState,
    private val census: NodeCensus,
    private val actors: ZLinkActorClient,
    private val topology: SampleTopology,
) : ZLinkSuspendingSpot<PlayerActor>() {
    private val kotlinActors = actors.kotlin()

    private val residents = mutableMapOf<String, PlayerActor>()
    private val pending = mutableMapOf<String, Messages.EnterZoneReq>()
    private val borders = mutableMapOf<String, BorderSnapshot>()
    private var tickValue = 0L
    private var tickTimer: ZLinkTimer? = null
    private var botTimer: ZLinkTimer? = null

    private data class BorderSnapshot(val tick: Long, val players: List<Messages.PlayerView>)

    override fun configure() {
        // The topic selects the two incoming routes for this Zone Spot, so payload handling
        // does not repeat that routing decision by filtering on its destination zone.
        ZoneWorldSpec.adjacentZones(context.spotId()).forEach { fromZoneId ->
            context
                .handlers()
                .addHandler(BorderSubscriptionHandlers.forRoute(fromZoneId, context.spotId()))
        }
    }

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse =
        ZLinkSpotCreateResponse.accept()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<Messages.EnterZoneReq>()
        val zone = context.spotId()
        if (actorId != join.playerId || zone != ZoneWorldSpec.zoneOf(join.x, join.y)) {
            return ZLinkSpotActorJoinResult.reject(Messages.EnterZoneRes(zone, "InvalidZone"))
        }
        // --8<-- [start:doc-zw-admission]
        if (maintenance.rejectsArrival(topology.nodeValue(), zone, join.fromZoneId)) {
            return ZLinkSpotActorJoinResult.reject(Messages.EnterZoneRes(zone, "ZoneMaintenance"))
        }
        if (join.crashBoundaryProbe) {
            println("crash-boundary join pending zone=$zone actor=$actorId")
            awaitCancellation()
        }
        pending[actorId] = join
        return ZLinkSpotActorJoinResult.accept(Messages.EnterZoneRes(zone))
        // --8<-- [end:doc-zw-admission]
    }

    override suspend fun onJoinedActorSuspending(actor: PlayerActor) {
        val join = pending.remove(actor.actorId) ?: return
        actor.applyAtZone(join.x, join.y, context.spotId(), join.isBot)
        residents[actor.actorId] = actor
        census.record(context.spotId(), residents.size)
        println(
            "zone actor joined zone=${context.spotId()} actor=${actor.actorId} generation=${actor.context().objectGeneration()} " +
                "player=${actor.actorId}, bot=${actor.isBot}, initial=${join.initialEntry}"
        )
        if (!actor.isBot && !join.initialEntry)
            actor.send(Messages.ZoneChangedNotify(actor.actorId, context.spotId()))
    }

    override suspend fun onLeaveActorSuspending(actor: PlayerActor) {
        residents.remove(actor.actorId, actor)
        census.record(context.spotId(), residents.size)
        println("zone actor left zone=${context.spotId()} actor=${actor.actorId}")
    }

    override suspend fun onDisconnectActorSuspending(actor: PlayerActor) {
        residents.remove(actor.actorId, actor)
        census.record(context.spotId(), residents.size)
        println("zone actor disconnected zone=${context.spotId()} actor=${actor.actorId}")
    }

    override suspend fun onInitializeSuspending() {
        census.hostZone(context.spotId())
        val tick =
            context.addTimer<ZoneTickHandler>(
                "zone-tick",
                Duration.ofMillis(ZoneWorldSpec.TICK_PERIOD_MS),
                systems.zlink.framework.spots.ZLinkTimerOptions(
                    systems.zlink.framework.spots.ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS,
                    1,
                    false,
                ),
            )
        val bots =
            context.addTimer<ZoneBotTickHandler>(
                "zone-bot-tick",
                Duration.ofMillis(ZoneWorldSpec.BOT_TICK_PERIOD_MS),
                systems.zlink.framework.spots.ZLinkTimerOptions(
                    systems.zlink.framework.spots.ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS,
                    1,
                    false,
                ),
            )
        tickTimer = tick.await()
        botTimer = bots.await()
    }

    override suspend fun onClosingSuspending(
        context: systems.zlink.framework.spots.ZLinkSpotClosingContext
    ) {
        census.releaseZone(this.context.spotId())
        tickTimer?.cancel()?.await()
        botTimer?.cancel()?.await()
    }

    suspend fun tick() {
        tickValue++
        borders.entries.removeIf { tickValue - it.value.tick > ZoneWorldSpec.BORDER_EXPIRY_TICKS }
        publishBorders()
        try {
            residents.values
                .toList()
                .filterNot { it.isBot }
                .forEach { actor ->
                    kotlinActors
                        .sendToActor(
                            actor.actorId,
                            Messages.DeliverZoneStateMsg(
                                context.spotId(),
                                tickValue,
                                statePlayers(),
                            ),
                        )
                        .await()
                }
        } catch (error: RuntimeException) {
            println("zone tick delivery error zone=${context.spotId()} detail=${error.message}")
        }
    }

    suspend fun botTick() {
        residents.values
            .toList()
            .filter { it.isBot && !it.pending }
            .forEach { actor ->
                kotlinActors.sendToActor(actor.actorId, Messages.BotTickMsg()).await()
            }
    }

    suspend fun move(actor: PlayerActor, targetX: Int, targetY: Int) {
        // --8<-- [start:doc-zw-move]
        val decision = ZoneWorldSpec.validateMove(actor.x, actor.y, targetX, targetY)
        if (!decision.accepted) {
            if (!actor.isBot)
                actor.send(
                    Messages.MoveRejectedNotify(decision.reason ?: "Rejected", actor.x, actor.y)
                )
            return
        }
        val targetZone = ZoneWorldSpec.zoneOf(targetX, targetY)
        if (!decision.zoneChanged) {
            actor.updatePosition(targetX, targetY)
            context
                .outbound()
                .kotlin()
                .sendToSpot(
                    context.spotId(),
                    Messages.UpdatePositionMsg(actor.actorId, targetX, targetY, actor.isBot),
                )
                .await()
            if (!actor.isBot)
                kotlinActors
                    .sendToActor(
                        actor.actorId,
                        Messages.DeliverZoneStateMsg(context.spotId(), tickValue, statePlayers()),
                    )
                    .await()
            return
        }
        // --8<-- [start:doc-zw-zone-change]
        actor.prepareMove(targetX, targetY, targetZone)
        actor
            .context()
            .joinSpot(
                targetZone,
                Messages.EnterZoneReq(
                    actor.actorId,
                    targetX,
                    targetY,
                    actor.isBot,
                    false,
                    actor.zoneId,
                    false,
                ),
            )
            .timeout(Duration.ofSeconds(10))
            .defer()
        // --8<-- [end:doc-zw-zone-change]
        println(
            "zone transfer requested actor=${actor.actorId} from=${context.spotId()} to=$targetZone node=${topology.nodeValue()}"
        )
        return
        // --8<-- [end:doc-zw-move]
    }

    suspend fun crashProbe(actor: PlayerActor, targetX: Int, targetY: Int) {
        val decision = ZoneWorldSpec.validateMove(actor.x, actor.y, targetX, targetY)
        if (!decision.accepted || !decision.zoneChanged)
            throw IllegalArgumentException("Crash probe requires one legal cross-zone move")
        val targetZone = ZoneWorldSpec.zoneOf(targetX, targetY)
        actor.prepareCrashProbe(targetX, targetY, targetZone)
        actor
            .context()
            .joinSpot(
                targetZone,
                Messages.EnterZoneReq(
                    actor.actorId,
                    targetX,
                    targetY,
                    actor.isBot,
                    false,
                    actor.zoneId,
                    true,
                ),
            )
            .timeout(Duration.ofSeconds(30))
            .defer()
    }

    fun applyPosition(update: Messages.UpdatePositionMsg) {
        residents[update.playerId]?.updatePosition(update.x, update.y)
    }

    // --8<-- [start:doc-zw-state-push]
    suspend fun deliverState(actor: PlayerActor, message: Messages.DeliverZoneStateMsg) {
        if (residents[actor.actorId] !== actor) return
        actor.send(Messages.ZoneStateNotify(message.zoneId, message.tick, message.players))
    }

    // --8<-- [end:doc-zw-state-push]
    fun applyBorder(event: Messages.ZoneBorderEvent) {
        val current = borders[event.fromZoneId]
        if (current == null || event.tick >= current.tick)
            borders[event.fromZoneId] = BorderSnapshot(event.tick, event.players.toList())
    }

    suspend fun announce(message: Messages.DeliverAnnounceMsg) {
        println(
            "zone spot: announcement delivered zone=${context.spotId()} id=${message.announcementId}"
        )
        residents.values
            .filterNot { it.isBot }
            .forEach { it.send(Messages.WorldAnnounceNotify(message.announcementId, message.text)) }
    }

    fun statePlayers(): List<Messages.PlayerView> {
        val values =
            residents.values
                .associate {
                    it.actorId to
                        Messages.PlayerView(it.actorId, it.x, it.y, context.spotId(), it.isBot)
                }
                .toMutableMap()
        borders.values.flatMap { it.players }.forEach { values.putIfAbsent(it.playerId, it) }
        return values.values.sortedWith(compareBy(ZoneWorldSpec.utf8Order) { it.playerId })
    }

    // --8<-- [start:doc-zw-border-publish]
    private suspend fun publishBorders() {
        ZoneWorldSpec.adjacentZones(context.spotId()).forEach { target ->
            val players =
                residents.values
                    .filter { ZoneWorldSpec.inBorderBand(it.x, it.y, context.spotId(), target) }
                    .map { Messages.PlayerView(it.actorId, it.x, it.y, context.spotId(), it.isBot) }
                    .sortedWith(compareBy(ZoneWorldSpec.utf8Order) { it.playerId })
            context
                .outbound()
                .kotlin()
                .publish(
                    ZoneWorldNames.ZONE_CHANNEL,
                    ZoneWorldNames.borderTopic(context.spotId(), target),
                    Messages.ZoneBorderEvent(context.spotId(), target, tickValue, players),
                )
                .await()
        }
    }
    // --8<-- [end:doc-zw-border-publish]
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ZoneMoveHandler :
    ZLinkSuspendingSpotActorSendHandler<ZoneSpot, PlayerActor, Messages.MoveMsg> {
    @ZLinkSpotActorSend
    override suspend fun handle(
        spot: ZoneSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: Messages.MoveMsg,
    ) = spot.move(actor, message.x, message.y)
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ZoneBotMoveHandler :
    ZLinkSuspendingSpotActorSendHandler<ZoneSpot, PlayerActor, Messages.BotTickMsg> {
    @ZLinkSpotActorSend
    override suspend fun handle(
        spot: ZoneSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: Messages.BotTickMsg,
    ) {
        val x = actor.x + actor.dirX * ZoneWorldSpec.BOT_STEP
        val y = actor.y + actor.dirY * ZoneWorldSpec.BOT_STEP
        if (!ZoneWorldSpec.validateMove(actor.x, actor.y, x, y).accepted) {
            actor.reverseDirection()
            return
        }
        return spot.move(actor, x, y)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ZoneJoinHandler :
    ZLinkSuspendingSpotActorSendHandler<ZoneSpot, PlayerActor, Messages.JoinWorldMsg> {
    @ZLinkSpotActorSend
    override suspend fun handle(
        spot: ZoneSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: Messages.JoinWorldMsg,
    ) {
        if (actor.pending) {
            throw IllegalStateException("Zone actor is not ready")
        }
        return actor.send(Messages.JoinWorldNotify(actor.actorId, actor.zoneId, actor.x, actor.y))
    }
}

class EntryZoneJoinHandler :
    ZLinkSuspendingEntrySpotActorSendHandler<ZoneEntrySpot, PlayerActor, Messages.JoinWorldMsg> {
    override suspend fun handle(
        entrySpot: ZoneEntrySpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: Messages.JoinWorldMsg,
    ) {
        require(actor.actorId == message.playerId) { "Join player does not match the actor" }
        actor.prepareEntry(ZoneWorldSpec.SPAWN_X, ZoneWorldSpec.SPAWN_Y, false, 0, 0)
        val zone = ZoneWorldSpec.zoneOf(ZoneWorldSpec.SPAWN_X, ZoneWorldSpec.SPAWN_Y)
        actor
            .context()
            .joinSpot(
                zone,
                Messages.EnterZoneReq(
                    actor.actorId,
                    ZoneWorldSpec.SPAWN_X,
                    ZoneWorldSpec.SPAWN_Y,
                    false,
                    true,
                    "",
                    false,
                ),
            )
            .timeout(Duration.ofSeconds(10))
            .defer()
    }
}

class EntryZoneEnterWorldHandler :
    ZLinkSuspendingEntrySpotActorRequestHandler<
        ZoneEntrySpot,
        PlayerActor,
        Messages.EnterWorldReq,
        Messages.EnterWorldRes,
    > {
    override suspend fun handle(
        entrySpot: ZoneEntrySpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        request: Messages.EnterWorldReq,
    ): Messages.EnterWorldRes {
        if (!ZoneWorldSpec.inRange(request.x, request.y))
            return Messages.EnterWorldRes("", request.x, request.y, "OutOfRange")
        // --8<-- [start:doc-zw-entry-join]
        actor.prepareEntry(request.x, request.y, request.isBot, request.dirX, request.dirY)
        val zone = ZoneWorldSpec.zoneOf(request.x, request.y)
        actor
            .context()
            .joinSpot(
                zone,
                Messages.EnterZoneReq(
                    actor.actorId,
                    request.x,
                    request.y,
                    request.isBot,
                    true,
                    "",
                    false,
                ),
            )
            .defer()
        // --8<-- [end:doc-zw-entry-join]
        return Messages.EnterWorldRes(zone, request.x, request.y)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class DeliverZoneStateHandler :
    ZLinkSuspendingSpotActorSendHandler<ZoneSpot, PlayerActor, Messages.DeliverZoneStateMsg> {
    @ZLinkSpotActorSend
    override suspend fun handle(
        spot: ZoneSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: Messages.DeliverZoneStateMsg,
    ) = spot.deliverState(actor, message)
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class UpdatePositionHandler :
    ZLinkSuspendingSpotPacketHandler<ZoneSpot, Messages.UpdatePositionMsg> {
    override suspend fun handle(spot: ZoneSpot, message: Messages.UpdatePositionMsg) {
        spot.applyPosition(message)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class DeliverAnnouncementHandler :
    ZLinkSuspendingSpotPacketHandler<ZoneSpot, Messages.DeliverAnnounceMsg> {
    override suspend fun handle(spot: ZoneSpot, message: Messages.DeliverAnnounceMsg) {
        spot.announce(message)
    }
}

class ZoneTickHandler(private val topology: SampleTopology, private val routes: ZLinkRouteClient) :
    ZLinkSuspendingSpotTimerHandler<ZoneSpot> {
    private val kotlinRoutes = routes.kotlin()

    override suspend fun handle(spot: ZoneSpot, tick: ZLinkTimerTick) {
        try {
            val fault = topology.faultTickZone
            if (
                (fault == "*" || fault == spot.context().spotId()) &&
                    faultInjected.compareAndSet(false, true)
            ) {
                error("injected tick failure for ZW-C4. zone=${spot.context().spotId()}")
            }
            spot.tick()
        } catch (error: RuntimeException) {
            val report =
                Messages.ReportSpotEventMsg(
                    topology.nodeValue(),
                    "TimerHandlerFailed",
                    "spot=${spot.context().spotId()}; timer=zone-tick; detail=${error.message}",
                    Instant.now().toString(),
                )
            try {
                kotlinRoutes.sendToChannel(ZoneWorldNames.REPORT_CHANNEL, report).await()
            } finally {
                throw error
            }
        }
    }

    companion object {
        private val faultInjected = AtomicBoolean()
    }
}

class ZoneBotTickHandler : ZLinkSuspendingSpotTimerHandler<ZoneSpot> {
    override suspend fun handle(spot: ZoneSpot, tick: ZLinkTimerTick) = spot.botTick()
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ProbeRequestHandler :
    ZLinkSuspendingSpotActorRequestHandler<
        ZoneSpot,
        PlayerActor,
        Messages.MessageFollowProbeReq,
        Messages.MessageFollowProbeRes,
    > {
    @ZLinkSpotActorRequest
    override suspend fun handle(
        spot: ZoneSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        request: Messages.MessageFollowProbeReq,
    ): Messages.MessageFollowProbeRes {
        println(
            "message-follow probe handled. actor=${actor.actorId}, probe=${request.probeId}, " +
                "payload=${HexFormat.of().withUpperCase().formatHex(request.payload)}"
        )
        return Messages.MessageFollowProbeRes(request.probeId, request.payload)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ProbeSendHandler :
    ZLinkSuspendingSpotActorSendHandler<ZoneSpot, PlayerActor, Messages.MessageFollowProbeMsg> {
    @ZLinkSpotActorSend
    override suspend fun handle(
        spot: ZoneSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: Messages.MessageFollowProbeMsg,
    ) {
        println(
            "message-follow probe one-way handled. actor=${actor.actorId}, probe=${message.probeId}, " +
                "payload=${HexFormat.of().withUpperCase().formatHex(message.payload)}"
        )
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ProbeCrashHandler :
    ZLinkSuspendingSpotActorSendHandler<ZoneSpot, PlayerActor, Messages.CrashRelocationProbeMsg> {
    @ZLinkSpotActorSend
    override suspend fun handle(
        spot: ZoneSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        message: Messages.CrashRelocationProbeMsg,
    ) = spot.crashProbe(actor, message.x, message.y)
}

// --8<-- [start:doc-zw-border-subscribe]
class BorderSubscriptionHandlers {
    companion object {
        fun forRoute(fromZoneId: String, toZoneId: String): Class<*> =
            when (ZoneWorldNames.borderTopic(fromZoneId, toZoneId)) {
                ZoneWorldNames.NW_NE -> NorthWestToNorthEast::class.java
                ZoneWorldNames.NW_SW -> NorthWestToSouthWest::class.java
                ZoneWorldNames.NE_NW -> NorthEastToNorthWest::class.java
                ZoneWorldNames.NE_SE -> NorthEastToSouthEast::class.java
                ZoneWorldNames.SW_NW -> SouthWestToNorthWest::class.java
                ZoneWorldNames.SW_SE -> SouthWestToSouthEast::class.java
                ZoneWorldNames.SE_NE -> SouthEastToNorthEast::class.java
                ZoneWorldNames.SE_SW -> SouthEastToSouthWest::class.java
                else -> error("unknown border route: $fromZoneId -> $toZoneId")
            }

        suspend fun apply(spot: ZoneSpot, event: Messages.ZoneBorderEvent) {
            spot.applyBorder(event)
        }
    }

    class NorthWestToNorthEast :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    class NorthWestToSouthWest :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    class NorthEastToNorthWest :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    class NorthEastToSouthEast :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    class SouthWestToNorthWest :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    class SouthWestToSouthEast :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    class SouthEastToNorthEast :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }

    class SouthEastToSouthWest :
        ZLinkSuspendingSpotSubscriptionHandler<ZoneSpot, Messages.ZoneBorderEvent> {
        override suspend fun handle(spot: ZoneSpot, event: Messages.ZoneBorderEvent) =
            apply(spot, event)
    }
}

// --8<-- [end:doc-zw-border-subscribe]

@ZLinkHandlerGroup(ZoneWorldNames.BROADCAST_HANDLER_GROUP)
class WorldAnnounceSubscriber(
    private val routes: ZLinkRouteClient,
    private val topology: SampleTopology,
    private val census: NodeCensus,
) : ZLinkSuspendingPublishHandler<Messages.WorldAnnounceEvent> {
    private val kotlinRoutes = routes.kotlin()

    override suspend fun handle(
        message: Messages.WorldAnnounceEvent,
        context: ZLinkPublishMessageContext,
    ) {
        println(
            "fanout subscriber received announcement node=${topology.nodeValue()} id=${message.announcementId}"
        )
        if (topology.isSubscriberOnly()) return
        census.zoneIds().forEach { zone ->
            kotlinRoutes
                .sendToSpot(zone, Messages.DeliverAnnounceMsg(message.announcementId, message.text))
                .await()
        }
    }
}

// --8<-- [start:doc-zw-maintenance-subscriber]
@ZLinkHandlerGroup(ZoneWorldNames.BROADCAST_HANDLER_GROUP)
class NodeMaintenanceSubscriber(
    private val state: NodeMaintenanceState,
    private val store: MaintenanceStore,
    private val topology: SampleTopology,
    private val statusReporter: ObjectProvider<ZoneStatusReporter>,
) : ZLinkSuspendingPublishHandler<Messages.NodeMaintenanceChangedEvent> {
    override suspend fun handle(
        message: Messages.NodeMaintenanceChangedEvent,
        context: ZLinkPublishMessageContext,
    ) {
        state.apply(message.nodeId, message.enabled)
        store.set(message.nodeId, message.enabled)
        if (topology.nodeValue() == message.nodeId) {
            println("maintenance state node=${message.nodeId} enabled=${message.enabled}")
            statusReporter.ifAvailable?.let { it.reportNow() }
        }
    }
}
// --8<-- [end:doc-zw-maintenance-subscriber]
