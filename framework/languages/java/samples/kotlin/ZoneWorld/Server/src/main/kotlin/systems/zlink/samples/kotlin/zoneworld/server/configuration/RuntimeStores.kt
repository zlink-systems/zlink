package systems.zlink.samples.kotlin.zoneworld.server.configuration

import io.lettuce.core.RedisClient
import io.lettuce.core.api.StatefulRedisConnection
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
    fun rejectsArrival(targetNode: String, sourceNode: String) =
        isUnderMaintenance(targetNode) && targetNode != sourceNode
}

class NodeCensus {
    private val counts = ConcurrentHashMap<String, Int>()
    fun record(zoneId: String, count: Int) { counts[zoneId] = count }
    fun total() = counts.values.sum()
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
    private val nodes = ConcurrentHashMap<String, Messages.NodeView>()
    private val routingIds = ConcurrentHashMap<String, String>()
    @Volatile private var live: Map<String, String> = emptyMap()

    @Synchronized fun applyLivePeers(observed: Map<String, String>) {
        live = observed.toMap()
        observed.forEach { (nodeId, routingId) -> apply(nodeId, routingId, true) }
        nodes.keys.toList().filterNot { observed.containsKey(it) }
            .forEach { apply(it, routingIds[it], false) }
    }

    @Synchronized fun report(report: Messages.ReportNodeStatusMsg) {
        val present = live.containsKey(report.nodeId)
        nodes[report.nodeId] = Messages.NodeView(
            report.nodeId, present, present, report.maintenance, report.zones, report.playerCount)
        println("report node=${report.nodeId} zones=${report.zones} players=${report.playerCount}")
    }

    fun alert(report: Messages.ReportSpotEventMsg) = println(
        "node alert node=${report.nodeId} kind=${report.kind} detail=${report.detail}")

    fun find(nodeId: String) = nodes[nodeId]
    fun snapshot() = nodes.values.sortedBy { it.nodeId }.map { it.copy(zones = it.zones.toList()) }

    private fun apply(nodeId: String, routingId: String?, present: Boolean) {
        val current = nodes[nodeId]
        if (present && routingId != null) routingIds[nodeId] = routingId
        // A node the console can no longer see tells it nothing about its own runtime
        // state, so the reported fields are dropped rather than kept as a stale row.
        val updated = if (present) {
            Messages.NodeView(
                nodeId, true, true, current?.maintenance == true,
                current?.zones ?: ZoneWorldSpec.zonesOf(nodeId), current?.playerCount ?: 0)
        } else {
            Messages.NodeView(nodeId, false, false, false, ZoneWorldSpec.zonesOf(nodeId), 0)
        }
        nodes[nodeId] = updated
        if (current != null &&
            current.registered == updated.registered &&
            current.connected == updated.connected
        ) {
            return
        }
        println(
            "node status observed. node=$nodeId" +
                ", rid=${routingIds[nodeId] ?: "none"}" +
                ", registered=${updated.registered}" +
                ", connected=${updated.connected}",
        )
    }
}
