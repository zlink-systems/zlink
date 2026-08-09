package systems.zlink.samples.kotlin.zoneworld.server.ops

import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSession
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.samples.kotlin.zoneworld.server.configuration.MaintenanceStore
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeRegistry
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec
class OpsSession(
    private val sessionContext: ZLinkSessionContext,
    private val registry: NodeRegistry,
    private val maintenance: MaintenanceStore,
    private val fanout: ZLinkFanoutClient,
) : ZLinkSession {
    override fun context() = sessionContext
    override fun onConnected(): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)
    override fun onDisconnected(): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)
    override fun onError(error: ZLinkStreamError): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)

    override fun onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): CompletionStage<Void> = when (dispatch.packetName()) {
        "WatchNodesReq" -> reply(Messages.WatchNodesRes(registry.snapshot()))
        "AnnounceWorldReq" -> announce(payload.decode(Messages.AnnounceWorldReq::class.java))
        "SetMaintenanceReq" -> setMaintenance(payload.decode(Messages.SetMaintenanceReq::class.java))
        "NodeDiagnosticsReq" -> diagnostics(payload.decode(Messages.NodeDiagnosticsReq::class.java))
        else -> error("unknown ZoneWorld Ops packet ${dispatch.packetName()}")
    }

    private fun announce(request: Messages.AnnounceWorldReq): CompletionStage<Void> {
        val id = UUID.randomUUID().toString()
        return fanout.publish(ZoneWorldNames.BROADCAST_CHANNEL, ZoneWorldNames.ANNOUNCE_TOPIC,
            Messages.WorldAnnounceEvent(id, request.text)).submit()
            .thenCompose { reply(Messages.AnnounceWorldRes(id)) }
    }

    private fun setMaintenance(request: Messages.SetMaintenanceReq): CompletionStage<Void> {
        val zones = ZoneWorldSpec.zonesOf(request.nodeId)
        if (zones.isEmpty()) return reply(Messages.SetMaintenanceRes(request.nodeId, false, emptyList(), "UnknownNode"))
        maintenance.set(request.nodeId, request.enabled)
        return fanout.publish(ZoneWorldNames.BROADCAST_CHANNEL, ZoneWorldNames.MAINTENANCE_TOPIC,
            Messages.NodeMaintenanceChangedEvent(request.nodeId, request.enabled)).submit()
            .thenCompose { reply(Messages.SetMaintenanceRes(request.nodeId, request.enabled, zones)) }
    }

    private fun diagnostics(request: Messages.NodeDiagnosticsReq): CompletionStage<Void> {
        val node = registry.find(request.nodeId)
        return if (node == null) reply(Messages.NodeDiagnosticsRes(request.nodeId, ZoneWorldSpec.zonesOf(request.nodeId), 0, maintenance.get(request.nodeId), "UnknownNode"))
        else reply(Messages.NodeDiagnosticsRes(node.nodeId, node.zones, node.playerCount, node.maintenance))
    }

    private fun reply(message: Any): CompletionStage<Void> = sessionContext.client().reply(message).submit()
}
