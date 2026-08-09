package systems.zlink.samples.kotlin.zoneworld.server.zone

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.ApplicationRunner
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkSendHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
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
    private val maintenance: NodeMaintenanceState,
    private val store: MaintenanceStore,
) : ApplicationRunner {
    override fun run(args: ApplicationArguments) {
        maintenance.apply(topology.nodeValue(), store.get(topology.nodeValue()))
        ZoneWorldSpec.zonesOf(topology.nodeValue()).forEach { zone ->
            spots.getOrCreate(zone, ZoneWorldNames.ZONE_SPOT_TYPE).inMesh(ZoneWorldNames.MESH).submit()
                .toCompletableFuture().join()
        }
        if (topology.nodeValue() == "zone-node-1") {
            ZoneWorldSpec.bots().forEach { bot ->
                actors.getOrCreate(bot.id, ZoneWorldNames.PLAYER_ACTOR_TYPE).inMesh(ZoneWorldNames.MESH)
                    .request(Messages.EnterWorldReq(bot.x, bot.y, true, bot.dirX, bot.dirY)).submit()
                    .toCompletableFuture().join()
            }
            println("bots=8 admitted")
        }
        println("topology=ready node=${topology.nodeValue()} zones=${ZoneWorldSpec.zonesOf(topology.nodeValue())}")
    }
}

class ZoneStatusReporter(
    private val topology: SampleTopology,
    private val routes: ZLinkRouteClient,
    private val census: NodeCensus,
    private val maintenance: NodeMaintenanceState,
) : AutoCloseable {
    private val scheduler: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "zoneworld-status-${topology.nodeValue()}").apply { isDaemon = true }
    }
    init { scheduler.scheduleAtFixedRate(::report, 0, 1, TimeUnit.SECONDS) }
    private fun report() {
        try {
            routes.sendToChannel(
                ZoneWorldNames.REPORT_CHANNEL,
                Messages.ReportNodeStatusMsg(
                    topology.nodeValue(), ZoneWorldSpec.zonesOf(topology.nodeValue()),
                    census.total(), maintenance.isUnderMaintenance(topology.nodeValue()),
                ),
            ).submit().whenComplete { _, error ->
                if (error != null) {
                    println("report failed node=${topology.nodeValue()} detail=${error.message}")
                }
            }
        } catch (error: RuntimeException) {
            // A fixed-rate task is cancelled when an invocation escapes. Ops
            // can start after a Zone node, so retain the periodic retry.
            println("report failed node=${topology.nodeValue()} detail=${error.message}")
        }
    }
    override fun close() { scheduler.shutdownNow() }
}

@ZLinkHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP)
class ReportNodeStatusHandler(private val registry: NodeRegistry) : ZLinkSendHandler<Messages.ReportNodeStatusMsg> {
    override fun handle(message: Messages.ReportNodeStatusMsg, context: ZLinkMessageContext): CompletionStage<Void> {
        registry.report(message); return CompletableFuture.completedFuture(null)
    }
}

@ZLinkHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP)
class ReportSpotEventHandler(private val registry: NodeRegistry) : ZLinkSendHandler<Messages.ReportSpotEventMsg> {
    override fun handle(message: Messages.ReportSpotEventMsg, context: ZLinkMessageContext): CompletionStage<Void> {
        registry.alert(message); return CompletableFuture.completedFuture(null)
    }
}
