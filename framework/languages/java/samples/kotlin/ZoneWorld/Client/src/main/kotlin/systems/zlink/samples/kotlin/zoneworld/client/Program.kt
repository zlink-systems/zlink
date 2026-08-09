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

suspend fun main(args: Array<String>) = coroutineScope {
    val options = ClientOptions.load(args)
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
        println("zoneworld-step=rejected-move")

        for (x in (movedX + 5)..48 step 5) {
            val target = minOf(x, 48)
            val position = async(start = CoroutineStart.UNDISPATCHED) {
                waitForPosition(game, "kotlin-zone-player", target, movedY).await()
            }
            game.send(Messages.MoveMsg(target, movedY)).await()
            position.await()
        }
        val changed = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.ZoneChangedNotify>()
                .where { it.payload().zoneId == "zone-ne" }
                .timeout(REQUEST_TIMEOUT)
                .await()
        }
        game.send(Messages.MoveMsg(52, movedY)).await()
        changed.await()
        waitForZone(game, "zone-ne")
        println("zoneworld-step=relocation")

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
        ensure(nodes.nodes.any { it.nodeId == "zone-node-2" }, "Ops observes zone-node-2")
        println("zoneworld-step=ops-nodes")

        val announcement = async(start = CoroutineStart.UNDISPATCHED) {
            game.waitFor<Messages.WorldAnnounceNotify>().timeout(REQUEST_TIMEOUT).await()
        }
        ops.request(Messages.AnnounceWorldReq("kotlin zoneworld announcement"))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.AnnounceWorldRes>()
        ensure(announcement.await().payload().text.isNotBlank(),
            "fanout announcement reaches the bound actor")

        val maintenanceOn = ops.request(Messages.SetMaintenanceReq("zone-node-2", true))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.SetMaintenanceRes>()
        ensure(maintenanceOn.enabled, "Ops enables node maintenance")
        val maintenanceOff = ops.request(Messages.SetMaintenanceReq("zone-node-2", false))
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.SetMaintenanceRes>()
        ensure(!maintenanceOff.enabled, "Ops disables node maintenance")

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

private suspend fun awaitNodes(connector: ZLinkKotlinStreamConnector): Messages.WatchNodesRes {
    val deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos()
    while (true) {
        val nodes = connector.request(Messages.WatchNodesReq())
            .timeout(REQUEST_TIMEOUT)
            .awaitReply<Messages.WatchNodesRes>()
        if (nodes.nodes.map { it.nodeId }.containsAll(listOf("zone-node-1", "zone-node-2"))) {
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
