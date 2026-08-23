package systems.zlink.samples.kotlin.zoneworld.server.ops

import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.CopyOnWriteArrayList
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
    private val consoles: OpsConsoleRegistry,
) : ZLinkSession {
    override fun context() = sessionContext
    override fun onConnected(): CompletionStage<Void> {
        consoles.add(sessionContext); return CompletableFuture.completedFuture(null)
    }
    override fun onDisconnected(): CompletionStage<Void> {
        consoles.remove(sessionContext); return CompletableFuture.completedFuture(null)
    }
    override fun onError(error: ZLinkStreamError): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)

    override fun onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): CompletionStage<Void> = when (dispatch.packetName()) {
        "WatchNodesReq" -> watch()
        "AnnounceWorldReq" -> announce(payload.decode(Messages.AnnounceWorldReq::class.java))
        "SetMaintenanceReq" -> setMaintenance(payload.decode(Messages.SetMaintenanceReq::class.java))
        "NodeDiagnosticsReq" -> diagnostics(payload.decode(Messages.NodeDiagnosticsReq::class.java))
        else -> error("unknown ZoneWorld Ops packet ${dispatch.packetName()}")
    }

    private fun watch(): CompletionStage<Void> {
        val nodes = registry.snapshot()
        return reply(Messages.WatchNodesRes(nodes)).thenCompose { consoles.replay(sessionContext, nodes) }
    }

    private fun announce(request: Messages.AnnounceWorldReq): CompletionStage<Void> {
        val id = UUID.randomUUID().toString()
        return fanout.publish(ZoneWorldNames.BROADCAST_CHANNEL, ZoneWorldNames.ANNOUNCE_TOPIC,
            Messages.WorldAnnounceEvent(id, request.text)).submit()
            .thenCompose { reply(Messages.AnnounceWorldRes(id)) }
    }

    private fun setMaintenance(request: Messages.SetMaintenanceReq): CompletionStage<Void> {
        val node = registry.find(request.nodeId)
            ?: return reply(Messages.SetMaintenanceRes(request.nodeId, false, emptyList(), "UnknownNode"))
        // The desired state is committed to the store; what the console shows for the node
        // stays whatever the node itself last reported, so an observed maintenance value is
        // always the node's own and never the operator's intent echoed back.
        maintenance.set(request.nodeId, request.enabled)
        return fanout.publish(ZoneWorldNames.BROADCAST_CHANNEL, ZoneWorldNames.MAINTENANCE_TOPIC,
            Messages.NodeMaintenanceChangedEvent(request.nodeId, request.enabled)).submit()
            .thenCompose { reply(Messages.SetMaintenanceRes(request.nodeId, request.enabled, node.zones)) }
    }

    private fun diagnostics(request: Messages.NodeDiagnosticsReq): CompletionStage<Void> {
        val node = registry.find(request.nodeId)
        return if (node == null) reply(Messages.NodeDiagnosticsRes(request.nodeId, emptyList(), 0, maintenance.get(request.nodeId), "UnknownNode"))
        else reply(Messages.NodeDiagnosticsRes(node.nodeId, node.zones, node.playerCount, node.maintenance))
    }

    private fun reply(message: Any): CompletionStage<Void> = sessionContext.client().reply(message).submit()
}

class OpsConsoleRegistry {
    private val sessions = CopyOnWriteArrayList<ZLinkSessionContext>()
    private val alerts = CopyOnWriteArrayList<Messages.NodeAlertNotify>()
    fun add(context: ZLinkSessionContext) { sessions += context }
    fun remove(context: ZLinkSessionContext) { sessions -= context }
    fun record(alert: Messages.NodeAlertNotify) { alerts += alert }
    fun broadcast(message: Any) {
        sessions.toList().forEach { it.client().send(message).submit().exceptionally { null } }
    }
    fun replay(context: ZLinkSessionContext, nodes: List<Messages.NodeView>): CompletionStage<Void> {
        var sends: CompletionStage<Void> = CompletableFuture.completedFuture(null)
        nodes.toList().forEach { node -> sends = sends.thenCompose {
            context.client().send(Messages.NodeStatusNotify(node.nodeId, node.registered, node.connected,
                node.maintenance, node.zones, node.playerCount)).submit()
        } }
        alerts.toList().forEach { alert -> sends = sends.thenCompose { context.client().send(alert).submit() } }
        return sends
    }
}
