package systems.zlink.samples.kotlin.zoneworld.server.ops

import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.decode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.samples.kotlin.zoneworld.server.configuration.MaintenanceStore
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeRegistry
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames

class OpsSession(
    private val sessionContext: ZLinkSessionContext,
    private val registry: NodeRegistry,
    private val maintenance: MaintenanceStore,
    private val fanout: ZLinkFanoutClient,
    private val consoles: OpsConsoleRegistry,
) : ZLinkSuspendingSession() {
    private val kotlinFanout = fanout.kotlin()

    override fun context() = sessionContext

    override suspend fun onConnectedSuspending() {
        consoles.add(sessionContext)
    }

    override suspend fun onDisconnectedSuspending() {
        consoles.remove(sessionContext)
    }

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {}

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        when (dispatch.packetName()) {
            "WatchNodesReq" -> watch()
            "AnnounceWorldReq" -> announce(payload.decode<Messages.AnnounceWorldReq>())
            "SetMaintenanceReq" -> setMaintenance(payload.decode<Messages.SetMaintenanceReq>())
            "NodeDiagnosticsReq" -> diagnostics(payload.decode<Messages.NodeDiagnosticsReq>())
            else -> error("unknown ZoneWorld Ops packet ${dispatch.packetName()}")
        }
    }

    private suspend fun watch() {
        val nodes = registry.snapshot()
        reply(Messages.WatchNodesRes(nodes))
        consoles.replay(sessionContext, nodes)
    }

    private suspend fun announce(request: Messages.AnnounceWorldReq) {
        val id = UUID.randomUUID().toString()
        kotlinFanout
            .publish(
                ZoneWorldNames.BROADCAST_CHANNEL,
                ZoneWorldNames.ANNOUNCE_TOPIC,
                Messages.WorldAnnounceEvent(id, request.text),
            )
            .await()
        reply(Messages.AnnounceWorldRes(id))
    }

    private suspend fun setMaintenance(request: Messages.SetMaintenanceReq) {
        val node = registry.find(request.nodeId)
        if (node == null) {
            reply(Messages.SetMaintenanceRes(request.nodeId, false, emptyList(), "UnknownNode"))
            return
        }
        maintenance.set(request.nodeId, request.enabled)
        kotlinFanout
            .publish(
                ZoneWorldNames.BROADCAST_CHANNEL,
                ZoneWorldNames.MAINTENANCE_TOPIC,
                Messages.NodeMaintenanceChangedEvent(request.nodeId, request.enabled),
            )
            .await()
        reply(Messages.SetMaintenanceRes(request.nodeId, request.enabled, node.zones))
    }

    private suspend fun diagnostics(request: Messages.NodeDiagnosticsReq) {
        val node = registry.find(request.nodeId)
        if (node == null) {
            reply(
                Messages.NodeDiagnosticsRes(
                    request.nodeId,
                    emptyList(),
                    0,
                    maintenance.get(request.nodeId),
                    "UnknownNode",
                )
            )
            return
        }
        reply(
            Messages.NodeDiagnosticsRes(node.nodeId, node.zones, node.playerCount, node.maintenance)
        )
    }

    private suspend fun reply(message: Any) =
        sessionContext.client().kotlin().reply(message).await()
}

class OpsConsoleRegistry : AutoCloseable {
    private val sessions = CopyOnWriteArrayList<ZLinkSessionContext>()
    private val alerts = CopyOnWriteArrayList<Messages.NodeAlertNotify>()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    fun add(context: ZLinkSessionContext) {
        sessions += context
    }

    fun remove(context: ZLinkSessionContext) {
        sessions -= context
    }

    fun record(alert: Messages.NodeAlertNotify) {
        alerts += alert
    }

    fun broadcast(message: Any) {
        sessions.toList().forEach { session ->
            scope.launch(start = CoroutineStart.UNDISPATCHED) {
                try {
                    session.client().kotlin().send(message).await()
                } catch (_: RuntimeException) {
                    // A stale console cannot prevent later best-effort submissions.
                }
            }
        }
    }

    override fun close() = scope.cancel()

    suspend fun replay(context: ZLinkSessionContext, nodes: List<Messages.NodeView>) {
        nodes.toList().forEach { node ->
            context
                .client()
                .kotlin()
                .send(
                    Messages.NodeStatusNotify(
                        node.nodeId,
                        node.registered,
                        node.connected,
                        node.maintenance,
                        node.zones,
                        node.playerCount,
                    )
                )
                .await()
        }
        alerts.toList().forEach { alert -> context.client().kotlin().send(alert).await() }
    }
}
