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

class NodeRegistry {
    private val nodes = ConcurrentHashMap<String, Messages.NodeView>()
    fun report(report: Messages.ReportNodeStatusMsg) {
        nodes[report.nodeId] = Messages.NodeView(
            report.nodeId, true, true, report.maintenance, report.zones, report.playerCount)
        println("report node=${report.nodeId} zones=${report.zones} players=${report.playerCount}")
    }
    fun alert(report: Messages.ReportSpotEventMsg) = println(
        "node alert node=${report.nodeId} kind=${report.kind} detail=${report.detail}")
    fun setMaintenance(nodeId: String, enabled: Boolean) {
        val current = nodes[nodeId]
        nodes[nodeId] = if (current == null) {
            Messages.NodeView(nodeId, false, false, enabled, ZoneWorldSpec.zonesOf(nodeId), 0)
        } else current.copy(maintenance = enabled)
    }
    fun find(nodeId: String) = nodes[nodeId]
    fun snapshot() = nodes.values.sortedBy { it.nodeId }.map { it.copy(zones = it.zones.toList()) }
}
