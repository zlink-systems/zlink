package systems.zlink.samples.kotlin.zoneworld.server.zone

import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.ApplicationRunner
import org.springframework.context.SmartLifecycle
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkRouteMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRouteSendHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToActor
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
    private val reporter: ZoneStatusReporter,
) : ApplicationRunner {
    private val kotlinSpots = spots.kotlin()
    private val kotlinActors = actors.kotlin()
    private val kotlinActorClient = actorClient.kotlin()

    // Ops learns a node's zone set only from the node's own status report (README §2.2). The
    // report is sent once the zone set is settled and before topology=ready is printed, so the
    // report an observer gates on never carries the pre-claim census; the periodic report and
    // the maintenance-change report are the only other senders.
    private suspend fun ready() {
        reporter.reportNow()
        println(
            "topology=ready node=${topology.nodeValue()} zones=${census.zoneIds().joinToString(",")}"
        )
    }

    override fun run(args: ApplicationArguments) = runBlocking {
        if (topology.isSubscriberOnly()) {
            println("topology=ready node=${topology.nodeValue()} zones=")
            return@runBlocking
        }
        // Maintenance is desired state, not a message: a node that starts reads it back from
        // the store, so a restart cannot quietly reopen a node the operator closed.
        val restored = store.get(topology.nodeValue())
        maintenance.apply(topology.nodeValue(), restored)
        println("maintenance restored node=${topology.nodeValue()} enabled=$restored")
        listOf("zone-node-1", "zone-node-2").forEach { nodeId ->
            maintenance.apply(nodeId, store.get(nodeId))
        }
        // A replacement keeps the NodeId and claims nothing. A stopped owner's zone objects stay
        // with the incarnation that owned them, so claiming here could settle on one zone — which
        // is neither the two a cold start needs nor the none a replacement announces, and a state
        // the loop below could never leave. Only a cold start claims.
        if (topology.allowsEmptyZoneSet()) {
            ready()
            return@runBlocking
        }
        var attempt = 0
        while (census.zoneIds().size != 2) {
            val claimed = census.zoneIds()
            val adjacentOrder = mutableListOf<String>()
            claimed.forEach { zone ->
                ZoneWorldSpec.adjacentZones(zone).forEach { adjacent ->
                    if (adjacent !in claimed && adjacent !in adjacentOrder)
                        adjacentOrder += adjacent
                }
            }
            val fallbackOrder =
                ZoneWorldSpec.zones().filter { it !in claimed && it !in adjacentOrder }
            var claimedChanged = false
            var adjacentSettling = false
            for (zone in adjacentOrder) {
                if (census.zoneIds() != claimed) {
                    claimedChanged = true
                    break
                }
                val result = runCatching {
                    kotlinSpots
                        .getOrCreate(zone, ZoneWorldNames.ZONE_SPOT_TYPE)
                        .inMesh(ZoneWorldNames.MESH)
                        .await()
                }
                if (
                    result.isFailure ||
                        census.zoneIds() == claimed &&
                            result.getOrNull()?.state() == ZLinkSpotCreateState.CREATED
                )
                    adjacentSettling = true
                if (census.zoneIds() != claimed) {
                    claimedChanged = true
                    break
                }
            }
            if (!claimedChanged && !adjacentSettling) {
                for (zone in fallbackOrder) {
                    if (census.zoneIds() != claimed) break
                    runCatching {
                        kotlinSpots
                            .getOrCreate(zone, ZoneWorldNames.ZONE_SPOT_TYPE)
                            .inMesh(ZoneWorldNames.MESH)
                            .await()
                    }
                    if (census.zoneIds() != claimed) break
                }
            }
            check(attempt++ < 119) {
                "Zone Spot capacity did not settle. node=${topology.nodeValue()} zones=${census.zoneIds()}"
            }
            delay(250)
        }
        if (!topology.botsDisabled()) {
            ZoneWorldSpec.bots()
                .filter { ZoneWorldSpec.zoneOf(it.x, it.y) in census.zoneIds() }
                .forEach { bot ->
                    val result =
                        kotlinActors
                            .getOrCreate(bot.id, ZoneWorldNames.PLAYER_ACTOR_TYPE)
                            .inMesh(ZoneWorldNames.MESH)
                            .request(ZLinkMessage.empty())
                            .await()
                    if (result is systems.zlink.framework.actors.ZLinkActorCreateResult.Created) {
                        kotlinActorClient
                            .requestToActor<Messages.EnterWorldRes>(
                                result.actor().actorId,
                                Messages.EnterWorldReq(bot.x, bot.y, true, bot.dirX, bot.dirY),
                            )
                            .await()
                    }
                    println(
                        "bot spawned. bot=${bot.id}, zone=${ZoneWorldSpec.zoneOf(bot.x, bot.y)}, " +
                            "start=(${bot.x},${bot.y}), dir=(${bot.dirX},${bot.dirY})"
                    )
                }
        }
        ready()
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
    private val kotlinRoutes = routes.kotlin()

    override fun start() =
        synchronized(lifecycleLock) {
            if (running) return@synchronized
            val createdScheduler =
                Executors.newSingleThreadScheduledExecutor { runnable ->
                    Thread(runnable, "zoneworld-status-${topology.nodeValue()}").apply {
                        isDaemon = true
                    }
                }
            scheduler = createdScheduler
            running = true
            createdScheduler.scheduleAtFixedRate(
                { runBlocking { report() } },
                ZoneWorldSpec.NODE_STATUS_REPORT_PERIOD_MS,
                ZoneWorldSpec.NODE_STATUS_REPORT_PERIOD_MS,
                TimeUnit.MILLISECONDS,
            )
        }

    suspend fun reportNow() = report()

    private suspend fun report() {
        val message =
            synchronized(lifecycleLock) {
                if (!running) return
                Messages.ReportNodeStatusMsg(
                    topology.nodeValue(),
                    census.zoneIds(),
                    census.total(),
                    maintenance.isUnderMaintenance(topology.nodeValue()),
                )
            }
        try {
            kotlinRoutes.sendToChannel(ZoneWorldNames.REPORT_CHANNEL, message).await()
            println("node status report submitted. node=${topology.nodeValue()}")
        } catch (error: RuntimeException) {
            // A fixed-rate task is cancelled when an invocation escapes. Ops can start after a
            // Zone node, so retain the periodic retry.
            println("report failed node=${topology.nodeValue()} detail=${error.message}")
        }
    }

    override fun stop() =
        synchronized(lifecycleLock) {
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
class ReportNodeStatusHandler(private val registry: NodeRegistry) :
    ZLinkSuspendingRouteSendHandler<Messages.ReportNodeStatusMsg> {
    override suspend fun handle(
        message: Messages.ReportNodeStatusMsg,
        context: ZLinkRouteMessageContext,
    ) {
        registry.report(message, context.sourceNodeRid().toString())
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP)
class ReportSpotEventHandler(private val registry: NodeRegistry) :
    ZLinkSuspendingSendHandler<Messages.ReportSpotEventMsg> {
    override suspend fun handle(
        message: Messages.ReportSpotEventMsg,
        context: ZLinkMessageContext,
    ) {
        registry.alert(message)
    }
}
