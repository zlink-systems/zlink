package systems.zlink.samples.kotlin.zoneworld.client

import java.net.URI
import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec
import systems.zlink.stream.connector.ZLinkStreamCompression
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

private val REQUEST_TIMEOUT = Duration.ofSeconds(15)

// A node lifecycle is a process lifecycle: the observation window has to cover a whole
// shutdown drain or a whole start, neither of which is a client round trip.
private val LIFECYCLE_TIMEOUT = Duration.ofSeconds(120)
private val MAINTENANCE_SETTLE_TIMEOUT = Duration.ofSeconds(5)
private const val EAST_NODE = "zone-node-2"

suspend fun main(args: Array<String>) {
    val options = ClientOptions.load(args)
    when (options.scenario) {
        "full" -> runFull(options)
        "lifecycle" -> runLifecycle(options)
        "replacement" -> runReplacement(options)
        else -> error("unknown ZoneWorld client scenario: ${options.scenario}")
    }
}

private suspend fun runFull(options: ClientOptions) = coroutineScope {
    val game = createConnector(options.gatewayEndpoint)
    val secondGame = createConnector(options.gatewayEndpoint)
    val ops = createConnector(options.opsEndpoint)
    try {
        game.connect().await()
        secondGame.connect().await()
        ops.connect().await()

        val firstReady = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.ZoneChangedNotify>()
                .where { it.payload().zoneId == "zone-nw" }
                .timeout(REQUEST_TIMEOUT)
                .await()
        }
        val first = game.request(Messages.JoinWorldReq("kotlin-zone-player"))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.JoinWorldRes>()
        ensure(first.x == ZoneWorldSpec.SPAWN_X && first.y == ZoneWorldSpec.SPAWN_Y,
            "spawn coordinates are authoritative")
        ensure(first.zoneId == "zone-nw", "spawn zone is zone-nw")
        firstReady.await()
        println("zoneworld-step=first-player-ready")

        val movedX = first.x + 3
        val movedY = first.y + 2
        val moved = async(start = CoroutineStart.UNDISPATCHED) {
            waitForPosition(game, "kotlin-zone-player", movedX, movedY).await()
        }
        game.send(Messages.MoveMsg(movedX, movedY)).await()
        moved.await()
        println("zoneworld-step=local-move")

        val rejected = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.MoveRejectedNotify>()
                .timeout(REQUEST_TIMEOUT)
                .await()
        }
        game.send(Messages.MoveMsg(-40, movedY)).await()
        ensure(rejected.await().payload().reason == "OutOfRange",
            "rejection order reports OutOfRange first")

        val tooFar = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.MoveRejectedNotify>()
                .timeout(REQUEST_TIMEOUT)
                .await()
        }
        game.send(Messages.MoveMsg(movedX + ZoneWorldSpec.MAX_STEP_PER_AXIS + 1, movedY)).await()
        ensure(tooFar.await().payload().reason == "TooFar",
            "an in-range oversized step reports TooFar")
        println("scenario ZW-A2 passed")
        println("zoneworld-step=rejected-move")

        walkEast(game, "kotlin-zone-player", movedX + 5, movedY)
        val changed = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.ZoneChangedNotify>()
                .where { it.payload().zoneId == "zone-ne" }
                .timeout(REQUEST_TIMEOUT)
                .await()
        }
        game.send(Messages.MoveMsg(52, movedY)).await()
        ensure(changed.await().payload().playerId == "kotlin-zone-player",
            "outbound relocation keeps the same player id")
        waitForZone(game, "zone-ne")
        println("zoneworld-step=relocation")

        val returned = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.ZoneChangedNotify>()
                .where { it.payload().zoneId == "zone-nw" }
                .timeout(REQUEST_TIMEOUT)
                .await()
        }
        game.send(Messages.MoveMsg(48, movedY)).await()
        ensure(returned.await().payload().playerId == "kotlin-zone-player",
            "return relocation keeps the same player id on the same session")
        // Settle on a coordinate no earlier walk has ever produced, so the matching
        // ZoneStateNotify provably postdates the return leg (binding continuity).
        val settleX = 44
        val settleY = movedY + 2
        val settled = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.ZoneStateNotify>()
                .where { state ->
                    state.payload().zoneId == "zone-nw" && state.payload().players.any { player ->
                        player.playerId == "kotlin-zone-player" &&
                            player.x == settleX && player.y == settleY
                    }
                }
                .timeout(REQUEST_TIMEOUT)
                .await()
        }
        game.send(Messages.MoveMsg(settleX, settleY)).await()
        settled.await()
        println("scenario ZW-B7 passed")
        println("zoneworld-step=return-relocation")

        val secondReady = async(start = CoroutineStart.UNDISPATCHED) {
            secondGame.waitFor<Messages.ZoneChangedNotify>()
                .where { it.payload().zoneId == "zone-nw" }
                .timeout(REQUEST_TIMEOUT)
                .await()
        }
        val second = secondGame.request(Messages.JoinWorldReq("kotlin-zone-player-b"))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.JoinWorldRes>()
        ensure(second.zoneId == "zone-nw", "second player joins the west zone")
        secondReady.await()
        println("zoneworld-step=second-player-ready")

        val state = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.ZoneStateNotify>().timeout(REQUEST_TIMEOUT).await()
        }
        val ids = state.await().payload().players.map { it.playerId }
        ensure(ids == ids.sortedWith(ZoneWorldSpec.utf8Order), "Players are UTF-8 sorted")
        println("zoneworld-step=sorted-state")

        val nodes = awaitNodes(ops)
        ensure(nodes.nodes.any { it.nodeId == "zone-node-1" }, "Ops observes zone-node-1")
        ensure(nodes.nodes.any { it.nodeId == EAST_NODE }, "Ops observes zone-node-2")
        println("zoneworld-step=ops-nodes")

        val announcement = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.WorldAnnounceNotify>().timeout(REQUEST_TIMEOUT).await()
        }
        ops.request(Messages.AnnounceWorldReq("kotlin zoneworld announcement"))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.AnnounceWorldRes>()
        ensure(announcement.await().payload().text.isNotBlank(),
            "fanout announcement reaches the bound actor")

        val duringMaintenance = applyMaintenance(ops, EAST_NODE, enabled = true)
        ensure(duringMaintenance.maintenance, "Ops observes maintenance=true on zone-node-2")
        val afterMaintenance = applyMaintenance(ops, EAST_NODE, enabled = false)
        ensure(!afterMaintenance.maintenance, "Ops observes maintenance=false on zone-node-2")
        println("zoneworld-step=maintenance-diagnostics")

        val diagnostics = ops.request(Messages.NodeDiagnosticsReq("zone-node-1"))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.NodeDiagnosticsRes>()
        ensure(diagnostics.error == null, "Ops diagnostics has no error")

        println("zoneworld server evidence=completed")
        println("zoneworld=completed")
    } finally {
        game.close().await()
        secondGame.close().await()
        ops.close().await()
    }
}

/**
 * ZW-E5 setup, ZW-C2 and ZW-C3. The runner stops the east node while this scenario is
 * watching: the console has to begin from a node that is registered and connected, or
 * "not registered" is indistinguishable from a node it has never heard of.
 */
private suspend fun runLifecycle(options: ClientOptions) {
    val ops = createConnector(options.opsEndpoint)
    try {
        ops.connect().await()

        // The operator closes the node before it is taken away, so the restart below has
        // to read the desired state back rather than come up open (ZW-E5).
        val armed = applyMaintenance(ops, EAST_NODE, enabled = true)
        ensure(armed.maintenance, "the east node reports itself closed before it stops")
        println("scenario ZW-E5 armed")

        // Both observations start from an established state, so the flip below is the
        // node going away and not the console's default for an unknown node.
        println("scenario ZW-C2 armed")
        println("scenario ZW-C3 armed")

        val gone = awaitNode(ops, EAST_NODE, LIFECYCLE_TIMEOUT, "unregistered") { !it.registered }
        ensure(!gone.registered, "a stopped node stops being registered")
        println("scenario ZW-C2 passed")

        val dropped = awaitNode(ops, EAST_NODE, LIFECYCLE_TIMEOUT, "disconnected") { !it.connected }
        ensure(!dropped.connected, "a node whose link drops is reported as disconnected")
        println("scenario ZW-C3 passed")
    } finally {
        ops.close().await()
    }
}

/**
 * ZW-E5 and the readiness half of ZW-G3. The runner has replaced the east node with a new
 * process for the same logical node on a different socket endpoint. Everything the console
 * knew about the old process was dropped when it left, so a maintenance value observed
 * here can only have been reported by the replacement.
 */
private suspend fun runReplacement(options: ClientOptions) {
    val ops = createConnector(options.opsEndpoint)
    try {
        ops.connect().await()

        val restored = awaitNode(
            ops, EAST_NODE, LIFECYCLE_TIMEOUT, "back under the maintenance it was closed with",
        ) { it.registered && it.connected && it.maintenance }
        ensure(restored.registered && restored.connected,
            "the replacement is observed as a registered, connected node")
        ensure(restored.maintenance, "the restarted node came up still under maintenance")
        println("scenario ZW-E5 passed")

        val reopened = applyMaintenance(ops, EAST_NODE, enabled = false)
        ensure(!reopened.maintenance, "the replacement reports itself reopened")
        ensure(reopened.zones == ZoneWorldSpec.zonesOf(EAST_NODE),
            "the replacement reports the zones the retired node held")
        println("scenario ZW-G3 ready")

        // A replacement that is merely present proves nothing. One publish with no node
        // list has to reach the new process and be accepted by the zone spots it built,
        // which the runner reads out of the replacement's own log.
        val announced = ops.request(Messages.AnnounceWorldReq("kotlin zoneworld replacement announcement"))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.AnnounceWorldRes>()
        ensure(announced.announcementId.isNotBlank(), "Ops publishes to the replaced topology")
        println("scenario ZW-G3 announced id=${announced.announcementId}")
    } finally {
        ops.close().await()
    }
}

private suspend fun walkEast(
    connector: ZLinkKotlinStreamConnector,
    playerId: String,
    startX: Int,
    y: Int,
) = coroutineScope {
    // Stops on the western side of the border, so the next step is one legal move across.
    var x = startX
    while (true) {
        val target = minOf(x, 48)
        val position = async(start = CoroutineStart.UNDISPATCHED) {
            waitForPosition(connector, playerId, target, y).await()
        }
        connector.send(Messages.MoveMsg(target, y)).await()
        position.await()
        if (target == 48) return@coroutineScope
        x += ZoneWorldSpec.MAX_STEP_PER_AXIS
    }
}

private fun waitForPosition(
    connector: ZLinkKotlinStreamConnector,
    playerId: String,
    x: Int,
    y: Int,
) = connector.waitFor<Messages.ZoneStateNotify>()
    .where { state ->
        state.payload().players.any { player ->
            player.playerId == playerId && player.x == x && player.y == y
        }
    }
    .timeout(REQUEST_TIMEOUT)

private suspend fun waitForZone(connector: ZLinkKotlinStreamConnector, zoneId: String) {
    connector.waitFor<Messages.ZoneStateNotify>()
        .where { it.payload().zoneId == zoneId }
        .timeout(REQUEST_TIMEOUT)
        .await()
}

/**
 * Commits a maintenance decision and waits until the node itself reports it. The decision
 * is stored, but the notification that carries it is a fanout publish and a subscriber that
 * is not attached yet never receives one, so the operator's request is re-issued until the
 * console observes the node in the requested state.
 */
private suspend fun applyMaintenance(
    connector: ZLinkKotlinStreamConnector,
    nodeId: String,
    enabled: Boolean,
): Messages.NodeView {
    val deadline = System.nanoTime() + LIFECYCLE_TIMEOUT.toNanos()
    while (true) {
        val applied = connector.request(Messages.SetMaintenanceReq(nodeId, enabled))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.SetMaintenanceRes>()
        ensure(applied.error == null && applied.enabled == enabled,
            "Ops accepts maintenance=$enabled for $nodeId")
        val settled = runCatching {
            awaitNode(
                connector, nodeId, MAINTENANCE_SETTLE_TIMEOUT, "reporting maintenance=$enabled",
            ) { it.registered && it.connected && it.maintenance == enabled }
        }
        settled.getOrNull()?.let { return it }
        check(System.nanoTime() < deadline) {
            "Ops never observed $nodeId reporting maintenance=$enabled"
        }
    }
}

private suspend fun awaitNode(
    connector: ZLinkKotlinStreamConnector,
    nodeId: String,
    timeout: Duration,
    description: String,
    accept: (Messages.NodeView) -> Boolean,
): Messages.NodeView {
    val deadline = System.nanoTime() + timeout.toNanos()
    while (true) {
        val nodes = connector.request(Messages.WatchNodesReq())
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.WatchNodesRes>()
        val node = nodes.nodes.firstOrNull { it.nodeId == nodeId }
        if (node != null && accept(node)) return node
        check(System.nanoTime() < deadline) { "Ops never observed $nodeId $description" }
        delay(100)
    }
}

private suspend fun awaitNodes(connector: ZLinkKotlinStreamConnector): Messages.WatchNodesRes {
    val deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos()
    while (true) {
        val nodes = connector.request(Messages.WatchNodesReq())
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.WatchNodesRes>()
        if (nodes.nodes.map { it.nodeId }.containsAll(listOf("zone-node-1", EAST_NODE))) {
            return nodes
        }
        check(System.nanoTime() < deadline) { "Ops node reports did not converge" }
        delay(100)
    }
}

private fun createConnector(endpoint: String): ZLinkKotlinStreamConnector =
    ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            REQUEST_TIMEOUT,
            REQUEST_TIMEOUT,
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            Int.MAX_VALUE,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            null,
            null,
            null,
        ),
    ).kotlin()

private fun ensure(condition: Boolean, message: String) {
    if (!condition) error(message)
}
