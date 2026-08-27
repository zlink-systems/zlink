package systems.zlink.samples.kotlin.zoneworld.server.zone

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.ApplicationRunner
import org.springframework.context.SmartLifecycle
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkRouteMessageContext
import systems.zlink.framework.channels.ZLinkRouteSendHandler
import systems.zlink.framework.channels.ZLinkSendHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotCreateState
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.zoneworld.server.configuration.MaintenanceStore
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeCensus
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeMaintenanceState
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeRegistry
import systems.zlink.samples.kotlin.zoneworld.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec
class ZoneBootstrap(
    private val topology: SampleTopology,
    private val spots: ZLinkSpotManager,
    private val actors: ZLinkActorManager,
    private val actorClient: systems.zlink.framework.actors.ZLinkActorClient,
    private val maintenance: NodeMaintenanceState,
    private val store: MaintenanceStore,
    private val census: NodeCensus,
) : ApplicationRunner {
    override fun run(args: ApplicationArguments) {
        if (topology.isSubscriberOnly()) {
            println("topology=ready node=${topology.nodeValue()} zones=")
            return
        }
        // Maintenance is desired state, not a message: a node that starts reads it back from
        // the store, so a restart cannot quietly reopen a node the operator closed.
        val restored = store.get(topology.nodeValue())
        maintenance.apply(topology.nodeValue(), restored)
        println("maintenance restored node=${topology.nodeValue()} enabled=$restored")
        listOf("zone-node-1", "zone-node-2").forEach { nodeId ->
            maintenance.apply(nodeId, store.get(nodeId))
        }
        var attempt = 0
        while (census.zoneIds().size != 2) {
            val claimed = census.zoneIds()
            val adjacentOrder = mutableListOf<String>()
            claimed.forEach { zone ->
                ZoneWorldSpec.adjacentZones(zone).forEach { adjacent ->
                    if (adjacent !in claimed && adjacent !in adjacentOrder) adjacentOrder += adjacent
                }
            }
            val fallbackOrder = ZoneWorldSpec.zones().filter { it !in claimed && it !in adjacentOrder }
            var claimedChanged = false
            var adjacentSettling = false
            for (zone in adjacentOrder) {
                if (census.zoneIds() != claimed) { claimedChanged = true; break }
                val result = runCatching {
                    spots.getOrCreate(zone, ZoneWorldNames.ZONE_SPOT_TYPE)
                        .inMesh(ZoneWorldNames.MESH).submit().toCompletableFuture().join()
                }
                if (result.isFailure ||
                    census.zoneIds() == claimed && result.getOrNull()?.state() == ZLinkSpotCreateState.CREATED
                ) adjacentSettling = true
                if (census.zoneIds() != claimed) { claimedChanged = true; break }
            }
            if (!claimedChanged && !adjacentSettling) {
                for (zone in fallbackOrder) {
                    if (census.zoneIds() != claimed) break
                    runCatching {
                        spots.getOrCreate(zone, ZoneWorldNames.ZONE_SPOT_TYPE)
                            .inMesh(ZoneWorldNames.MESH).submit().toCompletableFuture().join()
                    }
                    if (census.zoneIds() != claimed) break
                }
            }
            if (topology.allowsEmptyZoneSet() && census.zoneIds().isEmpty() && attempt >= 8) break
            check(attempt++ < 119) {
                "Zone Spot capacity did not settle. node=${topology.nodeValue()} zones=${census.zoneIds()}"
            }
            CompletableFuture.runAsync(
                {}, CompletableFuture.delayedExecutor(250, TimeUnit.MILLISECONDS),
            ).join()
        }
        if (!topology.botsDisabled()) {
            ZoneWorldSpec.bots().filter { ZoneWorldSpec.zoneOf(it.x, it.y) in census.zoneIds() }.forEach { bot ->
                val result = actors.getOrCreate(bot.id, ZoneWorldNames.PLAYER_ACTOR_TYPE)
                    .inMesh(ZoneWorldNames.MESH).request(ZLinkMessage.empty()).submit()
                    .toCompletableFuture().join()
                if (result is systems.zlink.framework.actors.ZLinkActorCreateResult.Created) {
                    actorClient.requestToActor(
                        result.actor().actorId,
                        Messages.EnterWorldReq(bot.x, bot.y, true, bot.dirX, bot.dirY),
                    ).submit(Messages.EnterWorldRes::class.java).toCompletableFuture().join()
                }
                println("bot spawned. bot=${bot.id}, zone=${ZoneWorldSpec.zoneOf(bot.x, bot.y)}, " +
                    "start=(${bot.x},${bot.y}), dir=(${bot.dirX},${bot.dirY})")
            }
        }
        println("topology=ready node=${topology.nodeValue()} zones=${census.zoneIds().joinToString(",")}")
    }
}

class ZoneStatusReporter(
    private val topology: SampleTopology,
    private val routes: ZLinkRouteClient,
    private val census: NodeCensus,
    private val maintenance: NodeMaintenanceState,
) : SmartLifecycle, AutoCloseable {
    private val lifecycleLock = Any()
    private var scheduler: ScheduledExecutorService? = null
    private var running = false

    override fun start() = synchronized(lifecycleLock) {
        if (running) return@synchronized
        val createdScheduler = Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "zoneworld-status-${topology.nodeValue()}").apply { isDaemon = true }
        }
        scheduler = createdScheduler
        running = true
        createdScheduler.scheduleAtFixedRate(
            ::report, 0, ZoneWorldSpec.NODE_STATUS_REPORT_PERIOD_MS, TimeUnit.MILLISECONDS,
        )
    }

    fun reportNow(): CompletionStage<Void> = report()

    private fun report(): CompletionStage<Void> = synchronized(lifecycleLock) {
        if (!running) return@synchronized CompletableFuture.completedFuture(null)
        try {
            return@synchronized routes.sendToChannel(
                ZoneWorldNames.REPORT_CHANNEL,
                Messages.ReportNodeStatusMsg(
                    topology.nodeValue(), census.zoneIds(),
                    census.total(), maintenance.isUnderMaintenance(topology.nodeValue()),
                ),
            ).submit().whenComplete { _, error ->
                if (error != null) {
                    println("report failed node=${topology.nodeValue()} detail=${error.message}")
                } else {
                    println("node status report submitted. node=${topology.nodeValue()}")
                }
            }
        } catch (error: RuntimeException) {
            // A fixed-rate task is cancelled when an invocation escapes. Ops
            // can start after a Zone node, so retain the periodic retry.
            println("report failed node=${topology.nodeValue()} detail=${error.message}")
            return@synchronized CompletableFuture.completedFuture(null)
        }
    }

    override fun stop() = synchronized(lifecycleLock) {
        running = false
        scheduler?.shutdownNow()
        scheduler = null
    }

    override fun stop(callback: Runnable) {
        try {
            stop()
        } finally {
            callback.run()
        }
    }

    override fun isRunning() = synchronized(lifecycleLock) { running }
    override fun getPhase() = 1
    override fun close() = stop()
}

@ZLinkHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP)
class ReportNodeStatusHandler(private val registry: NodeRegistry) : ZLinkRouteSendHandler<Messages.ReportNodeStatusMsg> {
    override fun handle(message: Messages.ReportNodeStatusMsg, context: ZLinkRouteMessageContext): CompletionStage<Void> {
        registry.report(message, context.sourceNodeRid().toString()); return CompletableFuture.completedFuture(null)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP)
class ReportSpotEventHandler(private val registry: NodeRegistry) : ZLinkSendHandler<Messages.ReportSpotEventMsg> {
    override fun handle(message: Messages.ReportSpotEventMsg, context: ZLinkMessageContext): CompletionStage<Void> {
        registry.alert(message); return CompletableFuture.completedFuture(null)
    }
}
