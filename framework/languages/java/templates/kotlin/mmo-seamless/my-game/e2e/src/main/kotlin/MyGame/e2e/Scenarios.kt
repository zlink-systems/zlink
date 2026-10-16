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
    try {
        val client = create()
        when (name) {
            Names.CROSS -> {
                val before = client.join(299)
                val session = client.sessionInfo()
                check(before.zoneId == "zone-3" && before.nodeId == "zoneA")
                val after = client.move(300, 1)
                check(after.zoneId == "zone-4" && after.nodeId == "zoneB")
                sameActor(session, client.sessionInfo())
                check(session == client.sessionInfo() && client.disconnects == 0)
                val continued = client.move(301, 2)
                check(continued.nodeId == "zoneB")
                sameActor(session, client.sessionInfo())
                check(client.dungeon("dungeon-${UUID.randomUUID()}").nodeId == "instance")
            }
            Names.BORDER_SCENARIO -> {
                val observer = create()
                val observed = observer.join(300)
                val origin = client.join(299)
                check(observed.nodeId == "zoneB" && origin.nodeId == "zoneA")
                client.move(298, 1)
                val border =
                    observer.next<BorderVisible> {
                        it.actorId == client.playerId && it.sequence == 1
                    }
                check(
                    border.observerZoneId == "zone-4" &&
                        border.sourceZoneId == "zone-3" &&
                        border.x == 298
                )
                check(observer.disconnects == 0)
            }
            Names.RECONNECT -> {
                client.join(299)
                val before = client.move(300, 1)
                val session = client.sessionInfo()
                client.close()
                clients.remove(client)
                val reconnect = create(client.playerId)
                val after = reconnect.join(0)
                check(before == after) { "Reconnect overwrote authoritative world state" }
                val rebound = reconnect.sessionInfo()
                sameActor(session, rebound)
                check(session.sessionId != rebound.sessionId) {
                    "Reconnect reused physical Session"
                }
                reconnect.move(301, 2)
                sameActor(session, reconnect.sessionInfo())
                check(reconnect.disconnects == 0)
            }
            Names.UPDATE ->
                withTimeout(Control.SAFE_DEADLINE.toMillis()) {
                    val before = client.join(299)
                    val session = client.sessionInfo()
                    check(before.nodeId == "zoneA")
                    client.move(298, 1)
                    // Independent IO lets gameplay continue during the host drain observation.
                    val drain = async(Dispatchers.IO) { nodes.drain(before.nodeId) }
                    var sequence = 1
                    var state: WorldState
                    do {
                        if (drain.isCompleted) drain.await()
                        sequence++
                        state = client.move(if (sequence % 2 == 0) 299 else 298, sequence)
                        sameActor(session, client.sessionInfo())
                        check(client.disconnects == 0)
                    } while (!drain.isCompleted)
                    drain.await()
                    check(state.zoneId == "zone-3" && state.nodeId == "zoneB")
                    nodes.replace(before.nodeId)
                    val after = client.move(299, sequence + 1)
                    check(
                        after.nodeId == "zoneB" &&
                            client.sessionInfo() == session &&
                            client.disconnects == 0
                    )
                    check(client.dungeon("after-update-${UUID.randomUUID()}").nodeId == "instance")
                    val replacement = create()
                    check(replacement.join(299).nodeId == "zoneB")
                    val fresh = replacement.sessionInfo()
                    check(replacement.move(300, 1).nodeId == "zoneB")
                    sameActor(fresh, replacement.sessionInfo())
                    check(replacement.disconnects == 0)
                }
            else -> error("Unknown scenario: $name")
        }
        if (name != Names.RECONNECT) check(client.disconnects == 0)
    } finally {
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
}
