package systems.zlink.samples.kotlin.zoneworld.server.zone

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.registerKotlinModule
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorFactory
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorJoinOperationId
import systems.zlink.framework.channels.ZLinkFanoutHandler
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.handlers.ZLinkSpotActorSend
import systems.zlink.framework.handlers.ZLinkSpotSubscription
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.framework.spots.ZLinkEntrySpot
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkSpot
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotPacketHandler
import systems.zlink.framework.spots.ZLinkSpotTimerHandler
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkTimer
import systems.zlink.framework.spots.ZLinkTimerTick
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter
import systems.zlink.framework.actors.ZLinkRelocationCancellation
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.samples.kotlin.zoneworld.server.configuration.MaintenanceStore
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeCensus
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeMaintenanceState
import systems.zlink.samples.kotlin.zoneworld.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec
class PlayerActor(
    val actorId: String,
    private val actorContext: ZLinkActorContext,
) : ZLinkActor {
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
    private val completedJoins = mutableSetOf<ZLinkActorJoinOperationId>()

    override fun context(): ZLinkActorContext = actorContext
    val pending: Boolean get() = pendingJoin
    val pendingTargetX: Int get() = pendingX
    val pendingTargetY: Int get() = pendingY
    val pendingTargetZone: String get() = pendingZone

    fun prepareEntry(x: Int, y: Int, bot: Boolean, dirX: Int, dirY: Int) {
        this.x = x; this.y = y; this.isBot = bot; this.dirX = dirX; this.dirY = dirY
        pendingX = x; pendingY = y; pendingZone = ZoneWorldSpec.zoneOf(x, y); pendingJoin = true
    }

    fun prepareMove(x: Int, y: Int, zone: String) {
        pendingX = x; pendingY = y; pendingZone = zone; pendingJoin = true
    }

    fun applyAtZone(x: Int, y: Int, zone: String, bot: Boolean) {
        this.x = x; this.y = y; zoneId = zone; isBot = bot; pendingJoin = false
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
    ) {
        this.x = x; this.y = y; zoneId = zone; isBot = bot
        this.dirX = dirX; this.dirY = dirY
        this.pendingX = pendingX; this.pendingY = pendingY
        this.pendingZone = pendingZone; this.pendingJoin = pendingJoin
    }

    fun updatePosition(x: Int, y: Int) { this.x = x; this.y = y }
    fun reverseDirection() { dirX = -dirX; dirY = -dirY }
    fun send(message: Any): CompletionStage<Void> = if (isBot) {
        CompletableFuture.completedFuture<Void>(null)
    } else actorContext.boundSession().send(message).submit()

    override fun onJoinCompleted(completion: ZLinkActorJoinCompletion): CompletionStage<Void> {
        val operationId = when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> completion.operationId()
            is ZLinkActorJoinCompletion.Rejected -> completion.operationId()
            is ZLinkActorJoinCompletion.Failed -> completion.operationId()
        }
        if (!completedJoins.add(operationId)) return CompletableFuture.completedFuture<Void>(null)
        pendingJoin = false
        val result: CompletionStage<Void> = when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> {
                val reply = completion.reply().decode(Messages.EnterZoneRes::class.java)
                val joinedZone = reply.zoneId.ifBlank { pendingZone.ifBlank { ZoneWorldSpec.zoneOf(pendingX, pendingY) } }
                applyAtZone(pendingX, pendingY, joinedZone, isBot)
                if (!isBot) send(Messages.ZoneChangedNotify(actorId, joinedZone))
                else CompletableFuture.completedFuture<Void>(null)
            }
            is ZLinkActorJoinCompletion.Rejected -> {
                if (!isBot) send(Messages.MoveRejectedNotify("ZoneMaintenance", x, y))
                else CompletableFuture.completedFuture<Void>(null)
            }
            is ZLinkActorJoinCompletion.Failed -> {
                if (!isBot) send(Messages.MoveRejectedNotify(completion.kind().name, x, y))
                else CompletableFuture.completedFuture<Void>(null)
            }
        }
        return result
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
    )
    override fun capture(actor: PlayerActor, cancellation: ZLinkRelocationCancellation): CompletionStage<ByteArray> =
        CompletableFuture.completedFuture(mapper.writeValueAsBytes(State(
            actor.x, actor.y, actor.zoneId, actor.isBot, actor.dirX, actor.dirY,
            actor.pendingTargetX, actor.pendingTargetY, actor.pendingTargetZone, actor.pending,
        )))
    override fun restore(actor: PlayerActor, state: ByteArray, cancellation: ZLinkRelocationCancellation): CompletionStage<Void> {
        val value = mapper.readValue(state, State::class.java)
        actor.restoreState(
            value.x, value.y, value.zone, value.bot, value.dirX, value.dirY,
            value.pendingX, value.pendingY, value.pendingZone, value.pendingJoin,
        )
        return CompletableFuture.completedFuture(null)
    }
}

class ZoneEntrySpot(private val context: ZLinkEntrySpotContext) : ZLinkEntrySpot<PlayerActor> {
    override fun context(): ZLinkEntrySpotContext = context
    override fun onCreateActor(actor: PlayerActor, createRequest: ZLinkMessage): CompletionStage<ZLinkActorCreateResponse> {
        val request = createRequest.decode(Messages.EnterWorldReq::class.java)
        if (!ZoneWorldSpec.inRange(request.x, request.y)) {
            return CompletableFuture.completedFuture(ZLinkActorCreateResponse.reject(
                Messages.EnterWorldRes("", request.x, request.y, "OutOfRange")))
        }
        val zone = ZoneWorldSpec.zoneOf(request.x, request.y)
        actor.prepareEntry(request.x, request.y, request.isBot, request.dirX, request.dirY)
        if (request.isBot) {
            actor.context().joinSpot(zone, Messages.EnterZoneMsg(actor.actorId, request.x, request.y,
                true, true, "")).timeout(Duration.ofSeconds(10)).defer()
        }
        return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept(
            Messages.JoinWorldRes(actor.actorId, zone, request.x, request.y)))
    }
    override fun onJoinedActor(actor: PlayerActor): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)
    override fun onLeaveActor(actor: PlayerActor): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)
}

class ZoneSpot(
    private val context: ZLinkSpotContext,
    private val maintenance: NodeMaintenanceState,
    private val census: NodeCensus,
    private val actors: ZLinkActorClient,
    private val topology: SampleTopology,
) : ZLinkSpot<PlayerActor> {
    override fun context(): ZLinkSpotContext = context
    private val residents = mutableMapOf<String, PlayerActor>()
    private val pending = mutableMapOf<String, Messages.EnterZoneMsg>()
    private val borders = mutableMapOf<String, BorderSnapshot>()
    private var tickValue = 0L
    private var tickTimer: ZLinkTimer? = null
    private var botTimer: ZLinkTimer? = null
    private data class BorderSnapshot(val tick: Long, val players: List<Messages.PlayerView>)

    override fun onCreate(request: ZLinkMessage): CompletionStage<ZLinkSpotCreateResponse> =
        CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept())

    override fun onActorJoin(actorId: String, request: ZLinkMessage): CompletionStage<ZLinkSpotActorJoinResult> {
        val join = request.decode(Messages.EnterZoneMsg::class.java)
        val zone = context.spotId()
        if (actorId != join.playerId || zone != ZoneWorldSpec.zoneOf(join.x, join.y)) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject(Messages.EnterZoneRes(zone, "InvalidZone")))
        }
        if (maintenance.rejectsArrival(ZoneWorldSpec.nodeOf(zone), join.fromNodeId)) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject(Messages.EnterZoneRes(zone, "ZoneMaintenance")))
        }
        pending[actorId] = join
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept(Messages.EnterZoneRes(zone)))
    }

    override fun onJoinedActor(actor: PlayerActor): CompletionStage<Void> {
        val join = pending.remove(actor.actorId) ?: return CompletableFuture.completedFuture(null)
        actor.applyAtZone(join.x, join.y, context.spotId(), join.isBot)
        residents[actor.actorId] = actor
        census.record(context.spotId(), residents.size)
        println("zone actor joined zone=${context.spotId()} actor=${actor.actorId} generation=${actor.context().objectGeneration()}")
        return CompletableFuture.completedFuture(null)
    }

    override fun onLeaveActor(actor: PlayerActor): CompletionStage<Void> {
        residents.remove(actor.actorId); census.record(context.spotId(), residents.size)
        println("zone actor left zone=${context.spotId()} actor=${actor.actorId}")
        return CompletableFuture.completedFuture(null)
    }

    override fun onDisconnectActor(actor: PlayerActor): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)

    override fun onInitialize(): CompletionStage<Void> {
        val tick = context.addTimer("zone-tick", Duration.ofMillis(ZoneWorldSpec.TICK_PERIOD_MS), ZoneTickHandler::class.java, null)
        val bots = context.addTimer("zone-bot-tick", Duration.ofMillis(ZoneWorldSpec.BOT_TICK_PERIOD_MS), ZoneBotTickHandler::class.java, null)
        return tick.thenCombine(bots) { first, second -> tickTimer = first; botTimer = second; null }
    }

    override fun onClosing(): CompletionStage<Void> {
        val first = tickTimer?.cancel() ?: CompletableFuture.completedFuture(null)
        val second = botTimer?.cancel() ?: CompletableFuture.completedFuture(null)
        return first.thenCombine(second) { _, _ -> null }
    }

    fun tick(): CompletionStage<Void> {
        tickValue++
        borders.entries.removeIf { tickValue - it.value.tick > ZoneWorldSpec.BORDER_EXPIRY_TICKS }
        publishBorders()
        var send: CompletionStage<Void> = CompletableFuture.completedFuture(null)
        residents.values.toList().filterNot { it.isBot }.forEach { actor ->
            send = send.thenCompose { actors.sendToActor(actor.actorId, Messages.DeliverZoneStateMsg(context.spotId(), tickValue, statePlayers())).submit() }
        }
        return send.exceptionally { println("zone tick delivery error zone=${context.spotId()} detail=${it.message}"); null }
    }

    fun botTick(): CompletionStage<Void> {
        var send: CompletionStage<Void> = CompletableFuture.completedFuture(null)
        residents.values.toList().filter { it.isBot && !it.pending }.forEach { actor ->
            send = send.thenCompose { actors.sendToActor(actor.actorId, Messages.BotTickMsg()).submit() }
        }
        return send
    }

    fun move(actor: PlayerActor, targetX: Int, targetY: Int): CompletionStage<Void> {
        val decision = ZoneWorldSpec.validateMove(actor.x, actor.y, targetX, targetY)
        if (!decision.accepted) return if (actor.isBot) CompletableFuture.completedFuture(null)
        else actor.send(Messages.MoveRejectedNotify(decision.reason ?: "Rejected", actor.x, actor.y))
        val targetZone = ZoneWorldSpec.zoneOf(targetX, targetY)
        if (!decision.zoneChanged) {
            actor.updatePosition(targetX, targetY)
            context.outbound().sendToSpot(context.spotId(), Messages.UpdatePositionMsg(actor.actorId, targetX, targetY, actor.isBot)).submit()
            return if (actor.isBot) CompletableFuture.completedFuture(null)
            else actors.sendToActor(actor.actorId, Messages.DeliverZoneStateMsg(context.spotId(), tickValue, statePlayers())).submit()
        }
        actor.prepareMove(targetX, targetY, targetZone)
        actor.context().joinSpot(targetZone, Messages.EnterZoneMsg(actor.actorId, targetX, targetY, actor.isBot, false, topology.nodeValue()))
            .timeout(Duration.ofSeconds(10)).defer()
        println("zone transfer requested actor=${actor.actorId} from=${context.spotId()} to=$targetZone node=${topology.nodeValue()}")
        return CompletableFuture.completedFuture(null)
    }

    fun applyPosition(update: Messages.UpdatePositionMsg) { residents[update.playerId]?.updatePosition(update.x, update.y) }
    fun deliverState(actor: PlayerActor, message: Messages.DeliverZoneStateMsg): CompletionStage<Void> {
        if (residents[actor.actorId] !== actor) return CompletableFuture.completedFuture(null)
        return actor.send(Messages.ZoneStateNotify(message.zoneId, message.tick, message.players))
    }
    fun applyBorder(event: Messages.ZoneBorderEvent) {
        if (event.toZoneId != context.spotId()) return
        val current = borders[event.fromZoneId]
        if (current == null || event.tick >= current.tick) borders[event.fromZoneId] = BorderSnapshot(event.tick, event.players.toList())
    }
    fun announce(message: Messages.DeliverAnnounceMsg) { residents.values.filterNot { it.isBot }.forEach { it.send(Messages.WorldAnnounceNotify(message.announcementId, message.text)) } }

    fun statePlayers(): List<Messages.PlayerView> {
        val values = residents.values.associate { it.actorId to Messages.PlayerView(it.actorId, it.x, it.y, context.spotId(), it.isBot) }.toMutableMap()
        borders.values.flatMap { it.players }.forEach { values.putIfAbsent(it.playerId, it) }
        return values.values.sortedWith(compareBy(ZoneWorldSpec.utf8Order) { it.playerId })
    }

    private fun publishBorders() {
        ZoneWorldSpec.adjacentZones(context.spotId()).forEach { target ->
            val players = residents.values.filter { ZoneWorldSpec.inBorderBand(it.x, it.y, context.spotId(), target) }
                .map { Messages.PlayerView(it.actorId, it.x, it.y, context.spotId(), it.isBot) }
                .sortedWith(compareBy(ZoneWorldSpec.utf8Order) { it.playerId })
            context.outbound().publish(ZoneWorldNames.ZONE_CHANNEL, ZoneWorldNames.borderTopic(context.spotId(), target),
                Messages.ZoneBorderEvent(context.spotId(), target, tickValue, players)).submit()
        }
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ZoneMoveHandler {
    @ZLinkSpotActorSend
    fun handle(spot: ZoneSpot, actor: PlayerActor, context: ZLinkMessageContext, message: Messages.MoveMsg): CompletionStage<Void> =
        spot.move(actor, message.x, message.y)
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ZoneBotMoveHandler {
    @ZLinkSpotActorSend
    fun handle(spot: ZoneSpot, actor: PlayerActor, context: ZLinkMessageContext, message: Messages.BotTickMsg): CompletionStage<Void> {
        val x = actor.x + actor.dirX * ZoneWorldSpec.BOT_STEP
        val y = actor.y + actor.dirY * ZoneWorldSpec.BOT_STEP
        if (!ZoneWorldSpec.validateMove(actor.x, actor.y, x, y).accepted) {
            actor.reverseDirection(); return CompletableFuture.completedFuture(null)
        }
        return spot.move(actor, x, y)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class ZoneJoinHandler {
    @ZLinkSpotActorRequest
    fun handle(spot: ZoneSpot, actor: PlayerActor, context: ZLinkMessageContext, message: Messages.JoinWorldReq): CompletionStage<Messages.JoinWorldRes> {
        if (actor.pending) {
            return CompletableFuture.failedFuture(IllegalStateException("Zone actor is not ready"))
        }
        return CompletableFuture.completedFuture(Messages.JoinWorldRes(actor.actorId, actor.zoneId, actor.x, actor.y))
    }
}

class EntryZoneJoinHandler : ZLinkEntrySpotActorRequestHandler<
    ZoneEntrySpot,
    PlayerActor,
    Messages.JoinWorldReq,
    Messages.JoinWorldRes,
> {
    override fun handle(
        entrySpot: ZoneEntrySpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        request: Messages.JoinWorldReq,
    ): CompletionStage<Messages.JoinWorldRes> {
        require(actor.actorId == request.playerId) { "Join player does not match the actor" }
        val zone = ZoneWorldSpec.zoneOf(actor.x, actor.y)
        actor.context().joinSpot(
            zone,
            Messages.EnterZoneMsg(actor.actorId, actor.x, actor.y, false, true, ""),
        ).timeout(Duration.ofSeconds(10)).defer()
        return CompletableFuture.completedFuture(
            Messages.JoinWorldRes(
                actor.actorId,
                zone,
                actor.x,
                actor.y,
            ),
        )
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class DeliverZoneStateHandler {
    @ZLinkSpotActorSend
    fun handle(spot: ZoneSpot, actor: PlayerActor, context: ZLinkMessageContext, message: Messages.DeliverZoneStateMsg): CompletionStage<Void> =
        spot.deliverState(actor, message)
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class UpdatePositionHandler : ZLinkSpotPacketHandler<ZoneSpot, Messages.UpdatePositionMsg> {
    override fun handle(spot: ZoneSpot, message: Messages.UpdatePositionMsg): CompletionStage<Void> {
        spot.applyPosition(message); return CompletableFuture.completedFuture(null)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class DeliverAnnouncementHandler : ZLinkSpotPacketHandler<ZoneSpot, Messages.DeliverAnnounceMsg> {
    override fun handle(spot: ZoneSpot, message: Messages.DeliverAnnounceMsg): CompletionStage<Void> {
        spot.announce(message); return CompletableFuture.completedFuture(null)
    }
}

class ZoneTickHandler : ZLinkSpotTimerHandler<ZoneSpot> {
    override fun handle(spot: ZoneSpot, tick: ZLinkTimerTick): CompletionStage<Void> = spot.tick()
}
class ZoneBotTickHandler : ZLinkSpotTimerHandler<ZoneSpot> {
    override fun handle(spot: ZoneSpot, tick: ZLinkTimerTick): CompletionStage<Void> = spot.botTick()
}

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
class BorderSubscriptionHandlers {
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_NE)
    fun nwNe(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> = apply(spot, event)
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_SW)
    fun nwSw(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> = apply(spot, event)
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_NW)
    fun neNw(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> = apply(spot, event)
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_SE)
    fun neSe(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> = apply(spot, event)
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_NW)
    fun swNw(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> = apply(spot, event)
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_SE)
    fun swSe(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> = apply(spot, event)
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_NE)
    fun seNe(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> = apply(spot, event)
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_SW)
    fun seSw(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> = apply(spot, event)
    private fun apply(spot: ZoneSpot, event: Messages.ZoneBorderEvent): CompletionStage<Void> {
        spot.applyBorder(event); return CompletableFuture.completedFuture(null)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.BROADCAST_HANDLER_GROUP)
class WorldAnnounceSubscriber(
    private val routes: ZLinkRouteClient,
    private val topology: SampleTopology,
) : ZLinkFanoutHandler<Messages.WorldAnnounceEvent> {
    override fun handle(message: Messages.WorldAnnounceEvent, context: ZLinkPublishMessageContext): CompletionStage<Void> {
        var send: CompletionStage<Void> = CompletableFuture.completedFuture(null)
        ZoneWorldSpec.zonesOf(topology.nodeValue()).forEach { zone ->
            send = send.thenCompose { routes.sendToSpot(zone, Messages.DeliverAnnounceMsg(message.announcementId, message.text)).submit() }
        }
        return send.thenRun { println("fanout announcement delivered node=${topology.nodeValue()} id=${message.announcementId}") }
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.BROADCAST_HANDLER_GROUP)
class NodeMaintenanceSubscriber(
    private val state: NodeMaintenanceState,
    private val store: MaintenanceStore,
) : ZLinkFanoutHandler<Messages.NodeMaintenanceChangedEvent> {
    override fun handle(message: Messages.NodeMaintenanceChangedEvent, context: ZLinkPublishMessageContext): CompletionStage<Void> {
        state.apply(message.nodeId, message.enabled); store.set(message.nodeId, message.enabled)
        println("maintenance state node=${message.nodeId} enabled=${message.enabled}")
        return CompletableFuture.completedFuture(null)
    }
}
