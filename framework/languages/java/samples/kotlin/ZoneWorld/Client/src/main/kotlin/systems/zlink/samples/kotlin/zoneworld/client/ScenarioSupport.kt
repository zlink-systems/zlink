package systems.zlink.samples.kotlin.zoneworld.client

import java.net.URI
import java.time.Duration
import java.util.UUID
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

internal val REQUEST_TIMEOUT: Duration = Duration.ofSeconds(15)
internal val CROSS_NODE_TIMEOUT: Duration = Duration.ofSeconds(30)

internal suspend fun connect(endpoint: String): ZLinkKotlinStreamConnector {
    val connector = ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create(endpoint), ZLinkStreamDispatchMode.IMMEDIATE,
            REQUEST_TIMEOUT, REQUEST_TIMEOUT, 2, Duration.ofSeconds(5),
            64 * 1024, 64 * 1024, Int.MAX_VALUE, 1024, 0, true,
            Duration.ofSeconds(1), Duration.ofSeconds(5), true,
            Duration.ofMillis(250), Duration.ofSeconds(5), 2.0, false,
            null, null, null, null,
        ),
    ).kotlin()
    connector.connect().await()
    return connector
}

internal fun ensure(condition: Boolean, message: String) {
    check(condition) { message }
}

internal fun unique(prefix: String) = "$prefix-${UUID.randomUUID().toString().replace("-", "").take(6)}"

internal data class Point(val x: Int, val y: Int)
internal data class Edge(val source: Point, val target: Point)

internal fun center(zone: String) = when (zone) {
    "zone-nw" -> Point(25, 25)
    "zone-ne" -> Point(75, 25)
    "zone-sw" -> Point(25, 75)
    "zone-se" -> Point(75, 75)
    else -> error("unknown zone $zone")
}

internal fun edge(source: String, target: String) = when ("$source:$target") {
    "zone-nw:zone-ne" -> Edge(Point(48, 25), Point(52, 25))
    "zone-nw:zone-sw" -> Edge(Point(25, 48), Point(25, 52))
    "zone-ne:zone-se" -> Edge(Point(75, 48), Point(75, 52))
    "zone-sw:zone-se" -> Edge(Point(48, 75), Point(52, 75))
    "zone-ne:zone-nw" -> Edge(Point(52, 25), Point(48, 25))
    "zone-sw:zone-nw" -> Edge(Point(25, 52), Point(25, 48))
    "zone-se:zone-ne" -> Edge(Point(75, 52), Point(75, 48))
    "zone-se:zone-sw" -> Edge(Point(52, 75), Point(48, 75))
    else -> error("zones are not adjacent")
}

internal interface AsyncCloseable { suspend fun close() }

internal suspend fun <T> withResources(
    vararg resources: AsyncCloseable,
    block: suspend () -> T,
): T = try {
    block()
} finally {
    resources.reversed().forEach { runCatching { it.close() } }
}

internal class Game private constructor(
    val connector: ZLinkKotlinStreamConnector,
    val playerId: String,
) : AsyncCloseable {
    var x = 0
    var y = 0

    suspend fun join(): Messages.JoinWorldRes = coroutineScope {
        val response = async(start = CoroutineStart.UNDISPATCHED) {
            connector.waitFor<Messages.JoinWorldRes>()
                .where { it.payload().playerId == playerId }
                .timeout(Duration.ofSeconds(20)).await().payload()
        }
        connector.send(Messages.JoinWorldReq(playerId)).await()
        response.await().also { x = it.x; y = it.y }
    }

    suspend fun move(targetX: Int, targetY: Int) {
        connector.send(Messages.MoveMsg(targetX, targetY)).await()
    }

    suspend fun moveTo(targetX: Int, targetY: Int): Messages.ZoneChangedNotify? = coroutineScope {
        var last: Messages.ZoneChangedNotify? = null
        while (x != targetX || y != targetY) {
            val nextX = if (x != targetX) x + (targetX - x).coerceIn(
                -ZoneWorldSpec.MAX_STEP_PER_AXIS, ZoneWorldSpec.MAX_STEP_PER_AXIS,
            ) else x
            val nextY = if (x == targetX) y + (targetY - y).coerceIn(
                -ZoneWorldSpec.MAX_STEP_PER_AXIS, ZoneWorldSpec.MAX_STEP_PER_AXIS,
            ) else y
            val oldZone = ZoneWorldSpec.zoneOf(x, y)
            val newZone = ZoneWorldSpec.zoneOf(nextX, nextY)
            if (oldZone != newZone) {
                val changed = async(start = CoroutineStart.UNDISPATCHED) {
                    connector.waitFor<Messages.ZoneChangedNotify>()
                        .where { it.payload().playerId == playerId && it.payload().zoneId == newZone }
                        .timeout(CROSS_NODE_TIMEOUT).await().payload()
                }
                move(nextX, nextY)
                last = changed.await()
            } else {
                val arrived = async(start = CoroutineStart.UNDISPATCHED) {
                    connector.waitFor<Messages.ZoneStateNotify>()
                        .where { state -> state.payload().players.any {
                            it.playerId == playerId && it.x == nextX && it.y == nextY
                        } }.timeout(CROSS_NODE_TIMEOUT).await()
                }
                move(nextX, nextY)
                arrived.await()
            }
            x = nextX; y = nextY
        }
        last
    }

    override suspend fun close() = connector.close().await()

    companion object {
        suspend fun create(options: ClientOptions, playerId: String) =
            Game(connect(options.gatewayEndpoint), playerId)
    }
}

internal class Ops private constructor(val connector: ZLinkKotlinStreamConnector) : AsyncCloseable {
    suspend fun watch(): Messages.WatchNodesRes = connector.request(Messages.WatchNodesReq())
        .timeout(REQUEST_TIMEOUT).awaitReply()

    suspend fun maintenance(nodeId: String, enabled: Boolean): Messages.SetMaintenanceRes = coroutineScope {
        val observed = async(start = CoroutineStart.UNDISPATCHED) {
            connector.waitFor<Messages.NodeStatusNotify>()
                .where { it.payload().nodeId == nodeId && it.payload().maintenance == enabled }
                .timeout(Duration.ofSeconds(20)).await()
        }
        val result = connector.request(Messages.SetMaintenanceReq(nodeId, enabled))
            .timeout(REQUEST_TIMEOUT).awaitReply<Messages.SetMaintenanceRes>()
        if (result.error == null) observed.await() else observed.cancel()
        result
    }

    override suspend fun close() = connector.close().await()

    companion object {
        suspend fun create(options: ClientOptions) = Ops(connect(options.opsEndpoint))
    }
}

internal class Probes private constructor(val connector: ZLinkKotlinStreamConnector) : AsyncCloseable {
    suspend fun pair(): Messages.RelocationPairRes = connector.request(Messages.RelocationPairReq())
        .timeout(REQUEST_TIMEOUT).awaitReply()
    suspend fun actor(id: String): Messages.ActorLocationProbeRes =
        connector.request(Messages.ActorLocationProbeReq(id)).timeout(REQUEST_TIMEOUT).awaitReply()
    suspend fun fresh(id: String): Messages.FreshActorProbeRes =
        connector.request(Messages.FreshActorProbeReq(id)).timeout(REQUEST_TIMEOUT).awaitReply()
    suspend fun probe(actor: String, id: String, payload: ByteArray): Messages.MessageFollowProbeRes =
        connector.request(Messages.MessageFollowProbeReq(actor, id, payload))
            .timeout(REQUEST_TIMEOUT).awaitReply()
    suspend fun sendProbe(actor: String, id: String, payload: ByteArray) =
        connector.send(Messages.MessageFollowProbeMsg(actor, id, payload)).await()
    override suspend fun close() = connector.close().await()

    companion object {
        suspend fun create(options: ClientOptions) = Probes(connect(options.gatewayEndpoint))
    }
}
