package MyGame.e2e

import MyGame.*
import java.util.UUID
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.withTimeout

private fun sameActor(before: SessionInfo, after: SessionInfo) {
    check(before.actorId == after.actorId) { "Actor ID changed" }
    check(before.objectGeneration == after.objectGeneration) { "Actor generation changed" }
}

suspend fun scenario(name: String, nodes: Nodes) = coroutineScope {
    val clients = mutableListOf<Client>()
    suspend fun create(playerId: String = "$name-${UUID.randomUUID()}"): Client {
        val client = Client(nodes.endpoint, playerId)
        clients.add(client)
        client.connect()
        return client
    }
    withCleanup(
        cleanup = {
            var failure: Exception? = null
            for (client in clients.asReversed()) {
                try {
                    client.close()
                } catch (error: Exception) {
                    if (failure == null) failure = error else failure.addSuppressed(error)
                }
            }
            failure?.let { throw it }
        }
    ) {
        val client = create()
        when (name) {
            Names.CROSS -> {
                val before = client.join(294.0)
                val session = client.sessionInfo()
                check(before.zoneId == "zone-3" && before.nodeId == "zoneA")
                val tooLarge = client.moveRejected(11.0, 1L)
                check(tooLarge.reason == "step-too-large" && tooLarge.retryAfterMs == 0)
                check(client.state() == before) { "Step rejection changed state" }
                val responses = client.moveWithImmediateBudgetRejection(2.0, 1L, 10.0, 2L)
                val first = responses.accepted
                check(first.x == 296.0)
                val budget = responses.rejected
                check(budget.reason == "move-budget" && budget.retryAfterMs > 0)
                check(client.state() == first) { "Move-budget rejection changed state" }
                val after = client.move(10.0, 2L)
                check(after.zoneId == "zone-4" && after.nodeId == "zoneB")
                sameActor(session, client.sessionInfo())
                val packet = client.packetRejected(EnterZone(Position(307.0), 3L, 0))
                check(packet.retryAfterMs == 0)
                check(client.state() == after) { "Session rejection changed state" }
                val edge = create("edge-${UUID.randomUUID()}")
                val edgeBefore = edge.join((Rules.WORLD_WIDTH - 1).toDouble())
                val outside = edge.moveRejected(1.0, 1L)
                check(outside.reason == "outside-world" && outside.retryAfterMs == 0)
                check(edge.state() == edgeBefore) { "World-boundary rejection changed state" }
                check(session == client.sessionInfo() && client.disconnects == 0)
                val continued = client.move(1.0, 3L)
                check(continued.nodeId == "zoneB")
                sameActor(session, client.sessionInfo())
                check(client.dungeon("dungeon-${UUID.randomUUID()}").nodeId == "instance")
            }
            Names.BORDER_SCENARIO -> {
                val observer = create()
                val observed = observer.join(300.0)
                val origin = client.join(299.0)
                check(observed.nodeId == "zoneB" && origin.nodeId == "zoneA")
                val appeared = observer.next<PlayerAppeared> { it.playerId == client.playerId }
                check(appeared.position.x == 299.0)
                client.move(-1.0, 1L)
                val moved = observer.next<PlayerMoved> {
                    it.playerId == client.playerId && it.sequence == 1L
                }
                check(moved.position.x == 298.0)
                client.move(2.0, 2L)
                val left = observer.next<PlayerLeft> {
                    it.playerId == client.playerId && it.reason == PlayerLeftReason.LEFT_ZONE
                }
                check(left.reason == PlayerLeftReason.LEFT_ZONE)
                val reappeared = observer.next<PlayerAppeared> {
                    it.playerId == client.playerId && it.position.x == 300.0
                }
                check(reappeared.position.x == 300.0)
                client.move(1.0, 3L)
                val movedAfterJoin = observer.next<PlayerMoved> {
                    it.playerId == client.playerId && it.sequence == 3L
                }
                check(movedAfterJoin.position.x == 301.0)
                client.close()
                clients.remove(client)
                val disconnected = observer.next<PlayerLeft> {
                    it.playerId == client.playerId && it.reason == PlayerLeftReason.DISCONNECTED
                }
                check(disconnected.reason == PlayerLeftReason.DISCONNECTED)
                check(observer.disconnects == 0)
            }
            Names.RECONNECT -> {
                client.join(299.0)
                val before = client.move(1.0, 1L)
                val session = client.sessionInfo()
                client.close()
                clients.remove(client)
                val reconnect = create(client.playerId)
                val after = reconnect.join(0.0)
                check(before == after) { "Reconnect overwrote authoritative world state" }
                val rebound = reconnect.sessionInfo()
                sameActor(session, rebound)
                check(session.sessionId != rebound.sessionId) {
                    "Reconnect reused physical Session"
                }
                reconnect.move(1.0, 2L)
                sameActor(session, reconnect.sessionInfo())
                check(reconnect.disconnects == 0)
            }
            Names.UPDATE ->
                withTimeout(Control.SAFE_DEADLINE.toMillis()) {
                    val before = client.join(299.0)
                    val session = client.sessionInfo()
                    check(before.nodeId == "zoneA")
                    var state = client.move(-1.0, 1L)
                    // Independent IO lets gameplay continue during the host drain observation.
                    val drain = async(Dispatchers.IO) { nodes.drain(before.nodeId) }
                    var sequence = 1L
                    do {
                        if (drain.isCompleted) drain.await()
                        sequence++
                        val target = if (sequence % 2 == 0L) 299.0 else 298.0
                        state = client.move(target - state.x, sequence)
                        sameActor(session, client.sessionInfo())
                        check(client.disconnects == 0)
                    } while (!drain.isCompleted)
                    drain.await()
                    check(state.zoneId == "zone-3" && state.nodeId == "zoneB")
                    nodes.replace(before.nodeId)
                    val after = client.move(299.0 - state.x, sequence + 1)
                    check(
                        after.nodeId == "zoneB" &&
                            client.sessionInfo() == session &&
                            client.disconnects == 0
                    )
                    check(client.dungeon("after-update-${UUID.randomUUID()}").nodeId == "instance")
                    val replacement = create()
                    check(replacement.join(Control.REPLACEMENT_X.toDouble()).nodeId == before.nodeId)
                    val fresh = replacement.sessionInfo()
                    check(replacement.move(1.0, 1L).nodeId == before.nodeId)
                    sameActor(fresh, replacement.sessionInfo())
                    check(replacement.disconnects == 0)
                }
            else -> error("Unknown scenario: $name")
        }
        for (activeClient in clients) check(activeClient.disconnects == 0)
    }
}
