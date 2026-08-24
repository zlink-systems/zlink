package systems.zlink.samples.kotlin.zoneworld.server.configuration

import io.lettuce.core.RedisClient
import io.lettuce.core.api.StatefulRedisConnection
import java.time.Duration
import java.util.concurrent.ConcurrentHashMap
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec
class MaintenanceStore(topology: SampleTopology) : AutoCloseable {
    private val client = RedisClient.create(
        topology.redisValue().let { endpoint ->
            if (endpoint.contains("://")) endpoint else "redis://$endpoint"
        },
    )
    private val connection: StatefulRedisConnection<String, String> = client.connect()
    private val redis = connection.sync()
    private val prefix = "${topology.prefixValue()}maintenance:"

    fun set(nodeId: String, enabled: Boolean) = redis.set("$prefix$nodeId", enabled.toString())
    fun get(nodeId: String) = redis.get("$prefix$nodeId" ).toBoolean()
    override fun close() { connection.close(); client.shutdown() }
}

class NodeMaintenanceState {
    private val values = ConcurrentHashMap<String, Boolean>()
    fun apply(nodeId: String, enabled: Boolean) { values[nodeId] = enabled }
    fun isUnderMaintenance(nodeId: String) = values[nodeId] == true
    fun rejectsArrival(ownNodeId: String, targetZoneId: String, sourceZoneId: String) =
        isUnderMaintenance(ownNodeId) && targetZoneId != sourceZoneId
}

class NodeCensus {
    private val counts = ConcurrentHashMap<String, Int>()
    fun hostZone(zoneId: String) { counts.putIfAbsent(zoneId, 0) }
    fun releaseZone(zoneId: String) { counts.remove(zoneId) }
    fun record(zoneId: String, count: Int) { counts.computeIfPresent(zoneId) { _, _ -> count } }
    fun total() = counts.values.sum()
    fun zoneIds() = counts.keys.sorted()
}

/**
 * What the Ops console knows about each node. Two sources feed it and neither is enough
 * alone: the mesh runtime says which nodes are present right now, and a node's own report
 * says which zones it holds, how many players are on it and whether it is closed.
 *
 * A node that has stopped is not there to answer a request, so registration and connection
 * are never polled - they follow the runtime peer observation. Everything the console
 * learned from the node itself is dropped when the node leaves, so a value read back after
 * a restart can only have come from the restarted process.
 */
class NodeRegistry {
    private data class State(val view: Messages.NodeView, val lastReportNanos: Long)
    private val reportTtlNanos = Duration.ofMillis(ZoneWorldSpec.NODE_STATUS_REPORT_TTL_MS).toNanos()
    private val nodes = ConcurrentHashMap<String, State>()
    private val nodeByRoutingId = ConcurrentHashMap<String, String>()
    private val routingIdByNode = ConcurrentHashMap<String, String>()
    @Volatile private var liveRoutingIds: Set<String> = emptySet()
    @Volatile private var changed: (Messages.NodeView) -> Unit = {}
    @Volatile private var alerted: (Messages.NodeAlertNotify) -> Unit = {}

    fun onChanged(handler: (Messages.NodeView) -> Unit) { changed = handler }
    fun onAlert(handler: (Messages.NodeAlertNotify) -> Unit) { alerted = handler }

    @Synchronized fun applyLiveRoutingIds(observed: Set<String>) {
        liveRoutingIds = observed.toSet()
        nodes.toMap().forEach { (nodeId, state) ->
            val connected = routingIdByNode[nodeId]?.let(observed::contains) == true
            if (state.view.connected != connected) {
                val updated = state.view.copy(connected = connected)
                nodes[nodeId] = State(updated, state.lastReportNanos)
                changed(updated)
                println("node connection observed. node=$nodeId, connected=$connected")
            }
        }
    }

    @Synchronized fun report(report: Messages.ReportNodeStatusMsg, routingId: String) {
        val now = System.nanoTime()
        routingIdByNode.put(report.nodeId, routingId)?.takeIf { it != routingId }?.let(nodeByRoutingId::remove)
        nodeByRoutingId[routingId] = report.nodeId
        val view = Messages.NodeView(report.nodeId, true, routingId in liveRoutingIds,
            report.maintenance, report.zones.toList(), report.playerCount)
        nodes[report.nodeId] = State(view, now); changed(view)
        println("node status observed. node=${report.nodeId}, rid=$routingId, registered=true, connected=${view.connected}")
    }

    @Synchronized fun expireStaleReports() {
        val now = System.nanoTime()
        nodes.toMap().forEach { (nodeId, state) ->
            if (state.view.registered && now - state.lastReportNanos >= reportTtlNanos) {
                val expired = state.view.copy(registered = false)
                nodes[nodeId] = State(expired, state.lastReportNanos); changed(expired)
            }
        }
    }

    fun alert(report: Messages.ReportSpotEventMsg) {
        val alert = Messages.NodeAlertNotify(report.nodeId, report.kind, report.detail, report.occurredAt)
        alerted(alert)
        println("node alert node=${report.nodeId} kind=${report.kind} detail=${report.detail}")
    }
    fun nodeIdOf(routingId: String) = nodeByRoutingId[routingId]
    fun routingIdOf(nodeId: String) = routingIdByNode[nodeId]
    fun find(nodeId: String) = nodes[nodeId]?.view
    fun snapshot(): List<Messages.NodeView> {
        expireStaleReports()
        return nodes.values.map { it.view }.sortedBy { it.nodeId }
    }
}
