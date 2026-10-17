package MyGame.e2e

import MyGame.*
import java.net.URI
import java.net.http.*
import java.util.UUID

object Scenarios {
    private const val EARNED_GOLD = 10L
    private const val CHANNEL_TWO = 2
    private const val CHANNEL_A1 = "channelA1"
    private const val CHANNEL_A2 = "channelA2"
    private const val CHANNEL_B = "channelB"

    suspend fun run(scenario: Scenario, nodes: Nodes) {
        when (scenario) {
            Scenario.WORLDS -> worlds(nodes)
            Scenario.CHANNEL -> channel(nodes)
            Scenario.RECONNECT -> reconnect(nodes)
            Scenario.UPDATE -> update(nodes)
        }
    }

    private fun player() = "player-${UUID.randomUUID()}"

    private fun view(value: PlayerView, world: String, channel: Int, node: String, gold: Long) {
        check(value == PlayerView(world, channel, node, gold)) { "Unexpected PlayerView: $value" }
    }

    private fun view(client: Client, world: String, channel: Int, node: String, gold: Long) {
        val expected = PlayerView(world, channel, node, gold)
        view(client.next(PlayerView::class.java) { it == expected }, world, channel, node, gold)
    }

    private suspend fun current(client: Client): Client {
        client.send(ViewRequest())
        return client
    }

    private fun connected(client: Client) {
        check(client.disconnects.get() == 0) { "Connection was interrupted" }
    }

    private suspend fun worlds(n: Nodes) {
        HttpClient.newHttpClient().use { http ->
            val reply =
                http.send(
                    HttpRequest.newBuilder(URI(n.accountsEndpoint + Names.WORLD_LIST_PATH))
                        .GET()
                        .build(),
                    HttpResponse.BodyHandlers.ofString(),
                )
            check(reply.statusCode() == 200) { "World list HTTP status" }
            val list = Json.mapper.readValue(reply.body(), Array<WorldInfo>::class.java)
            check(list.size == 2) { "Expected two worlds" }
            list.forEach { w ->
                check(
                    w.sessionEndpoint == n.endpoints[w.worldId] && w.name == "World ${w.worldId}"
                ) {
                    "Invalid world entry"
                }
            }
            check(list.map { it.worldId }.toSet() == setOf(Names.WORLD_A, Names.WORLD_B)) {
                "World identities differ"
            }
        }
        val id = player()
        withClient(n.endpoints.getValue(Names.WORLD_A), id) { a ->
            withClient(n.endpoints.getValue(Names.WORLD_B), id) { b ->
                view(a.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
                a.send(EarnRequest(EARNED_GOLD))
                view(a, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                view(b.connect(), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0)
                view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                connected(a)
                connected(b)
            }
        }
    }

    private suspend fun channel(n: Nodes) {
        withClient(n.endpoints.getValue(Names.WORLD_A), player()) { c ->
            view(c.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
            c.send(EarnRequest(EARNED_GOLD))
            view(c, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
            val identity = c.sessionInfo()
            c.send(ChangeChannelRequest(CHANNEL_TWO))
            view(c, Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD)
            check(identity == c.sessionInfo()) { "SessionInfo changed during channel transfer" }
            connected(c)
        }
    }

    private suspend fun reconnect(n: Nodes) {
        val id = player()
        val identity =
            withClient(n.endpoints.getValue(Names.WORLD_A), id) { c ->
                view(c.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
                c.send(EarnRequest(EARNED_GOLD))
                view(c, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                c.send(ChangeChannelRequest(CHANNEL_TWO))
                view(c, Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD)
                c.sessionInfo()
            }
        withClient(n.endpoints.getValue(Names.WORLD_A), id) { c ->
            view(c.connect(), Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD)
            val reconnected = c.sessionInfo()
            check(
                identity.actorId == reconnected.actorId &&
                    identity.objectGeneration == reconnected.objectGeneration
            ) {
                "Actor identity changed"
            }
            check(identity.sessionId != reconnected.sessionId) {
                "New connection reused Session ID"
            }
            connected(c)
        }
    }

    private suspend fun update(n: Nodes) {
        withClient(n.endpoints.getValue(Names.WORLD_A), player()) { a ->
            withClient(n.endpoints.getValue(Names.WORLD_B), player()) { b ->
                view(a.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
                view(b.connect(), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0)
                a.send(EarnRequest(EARNED_GOLD))
                view(a, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                val ai = a.sessionInfo()
                val bi = b.sessionInfo()
                n.drain(CHANNEL_A1)
                view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, EARNED_GOLD)
                view(current(b), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0)
                check(ai == a.sessionInfo() && bi == b.sessionInfo()) {
                    "Drain changed SessionInfo"
                }
                connected(a)
                connected(b)
                n.replace(CHANNEL_A1)
                view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, EARNED_GOLD)
                view(current(b), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0)
                check(ai == a.sessionInfo() && bi == b.sessionInfo()) {
                    "Replacement changed SessionInfo"
                }
                n.createUpdateZone(CHANNEL_A1)
                withClient(n.endpoints.getValue(Names.WORLD_A), player()) { fresh ->
                    view(fresh.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, 0)
                    fresh.send(ChangeChannelRequest(Names.UPDATE_CHANNEL))
                    view(fresh, Names.WORLD_A, Names.UPDATE_CHANNEL, CHANNEL_A1, 0)
                    connected(fresh)
                    fresh.send(EarnRequest(EARNED_GOLD))
                    view(fresh, Names.WORLD_A, Names.UPDATE_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                }
                connected(a)
                connected(b)
            }
        }
    }
}
