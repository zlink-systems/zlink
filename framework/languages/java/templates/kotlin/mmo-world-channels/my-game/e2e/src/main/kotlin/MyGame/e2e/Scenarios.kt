package MyGame.e2e

import MyGame.*
import java.net.URI
import java.net.http.*
import java.util.UUID

object Scenarios {
    private const val EARNED_GOLD = 10L
    private const val CHANNEL_TWO = 2L
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

    private fun view(value: PlayerView, world: String, channel: Long, node: String, gold: Long) {
        check(value == PlayerView(world, channel, node, gold)) { "Unexpected PlayerView: $value" }
    }

    private fun view(client: Client, world: String, channel: Long, node: String, gold: Long) {
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
                a.send(EarnRequest())
                view(a, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                a.send(EarnRequest())
                val rejected = a.rejection()
                check(
                    rejected.reason == Rejection.EARN_TOO_SOON && rejected.retryAfterMs > 0,
                ) { "Early earn was not rejected with a retry delay" }
                view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                a.waitRetryAfter(rejected.retryAfterMs)
                a.send(EarnRequest())
                view(a, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 2 * EARNED_GOLD)
                view(b.connect(), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0)
                view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 2 * EARNED_GOLD)
                connected(a)
                connected(b)
            }
        }
    }

    private suspend fun channel(n: Nodes) {
        val id = player()
        withClient(n.endpoints.getValue(Names.WORLD_A), id) { c ->
            withClient(n.endpoints.getValue(Names.WORLD_A), "$id-same") { same ->
                withClient(n.endpoints.getValue(Names.WORLD_A), "$id-moving") { moving ->
                    val initialMark = c.markChannelViews()
                    view(c.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
                    c.channelViewAfter(initialMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds == listOf(c.playerId)
                    }
                    c.send(EarnRequest())
                    view(c, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                    val sameAtCMark = c.markChannelViews()
                    val sameAtSelfMark = same.markChannelViews()
                    view(same.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
                    val expectedPair = setOf(c.playerId, same.playerId)
                    val sameAtC = c.channelViewAfter(sameAtCMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds.toSet() == expectedPair
                    }
                    val sameAtSelf = same.channelViewAfter(sameAtSelfMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds.toSet() == expectedPair
                    }
                    check(
                        sameAtC.playerIds.toSet() == expectedPair &&
                            sameAtSelf.playerIds.toSet() == expectedPair,
                ) { "Players in one Zone did not see the same player list" }

                    val allAtCMark = c.markChannelViews()
                    val allAtSameMark = same.markChannelViews()
                    val allAtMovingMark = moving.markChannelViews()
                    view(moving.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
                    val expectedAll = setOf(c.playerId, same.playerId, moving.playerId)
                    val allAtC = c.channelViewAfter(allAtCMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds.toSet() == expectedAll
                    }
                    val allAtSame = same.channelViewAfter(allAtSameMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds.toSet() == expectedAll
                    }
                    moving.channelViewAfter(allAtMovingMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds.toSet() == expectedAll
                    }
                    check(
                        allAtC.playerIds.toSet() == expectedAll &&
                            allAtSame.playerIds.toSet() == expectedAll,
                    ) { "Players in one Zone did not see each other" }

                    c.send(ChangeChannelRequest(GameRules.MAX_INTEGER + 1))
                    val invalidChannel = c.rejection()
                    check(
                        invalidChannel.reason == Rejection.INTEGER_OUT_OF_RANGE &&
                            invalidChannel.retryAfterMs == 0,
                    ) { "Oversized channel request was not rejected" }
                    check(c.channelView().playerIds.toSet() == expectedAll) {
                        "Oversized channel request changed membership"
                    }
                    c.send(PlayerView())
                    val internal = c.rejection()
                    check(
                        internal.reason == Rejection.PACKET_NOT_ALLOWED && internal.retryAfterMs == 0,
                    ) { "Session forwarded an internal packet" }

                    val identity = c.sessionInfo()
                    val oldAtCMark = c.markChannelViews()
                    val oldAtSameMark = same.markChannelViews()
                    val destinationMark = moving.markChannelViews()
                    moving.send(ChangeChannelRequest(CHANNEL_TWO))
                    view(moving, Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, 0)
                    val oldAtC = c.channelViewAfter(oldAtCMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds.toSet() == expectedPair
                    }
                    val oldAtSame = same.channelViewAfter(oldAtSameMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds.toSet() == expectedPair
                    }
                    val movedView = moving.channelViewAfter(destinationMark) {
                        it.channelId == CHANNEL_TWO && it.playerIds == listOf(moving.playerId)
                    }
                    check(
                        oldAtC.playerIds.toSet() == expectedPair &&
                            oldAtSame.playerIds.toSet() == expectedPair &&
                            movedView.playerIds == listOf(moving.playerId),
                ) { "Channel move left a player in the old list or exposed another channel" }

                    val disconnectedMark = c.markChannelViews()
                    same.close()
                    val afterDisconnect = c.channelViewAfter(disconnectedMark) {
                        it.channelId == Names.FIRST_CHANNEL && it.playerIds == listOf(c.playerId)
                    }
                    check(afterDisconnect.playerIds == listOf(c.playerId)) {
                        "Disconnected player remained in the channel list"
                    }

                    val expectedDestination = setOf(c.playerId, moving.playerId)
                    val movedMark = c.markChannelViews()
                    c.send(ChangeChannelRequest(CHANNEL_TWO))
                    view(c, Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD)
                    val movedViewForPrimary = c.channelViewAfter(movedMark) {
                        it.channelId == CHANNEL_TWO && it.playerIds.toSet() == expectedDestination
                    }
                    check(movedViewForPrimary.playerIds.toSet() == expectedDestination) {
                        "Channel move did not update the destination list"
                    }
                    check(identity == c.sessionInfo()) { "SessionInfo changed during channel transfer" }
                    connected(c)
                    connected(moving)
                }
            }
        }
    }

    private suspend fun reconnect(n: Nodes) {
        val id = player()
        val identity =
            withClient(n.endpoints.getValue(Names.WORLD_A), id) { c ->
                view(c.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
                c.send(EarnRequest())
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
                val initialMark = a.markChannelViews()
                view(a.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0)
                a.channelViewAfter(initialMark) {
                    it.channelId == Names.FIRST_CHANNEL && it.playerIds == listOf(a.playerId)
                }
                view(b.connect(), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0)
                a.send(EarnRequest())
                view(a, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                val ai = a.sessionInfo()
                val bi = b.sessionInfo()
                n.drain(CHANNEL_A1)
                view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, EARNED_GOLD)
                check(a.channelView().playerIds == listOf(a.playerId)) {
                    "Zone relocation lost its player list"
                }
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
                    check(fresh.channelView().playerIds == listOf(fresh.playerId)) {
                        "Replacement Zone did not include its joined player"
                    }
                    connected(fresh)
                    fresh.send(EarnRequest())
                    view(fresh, Names.WORLD_A, Names.UPDATE_CHANNEL, CHANNEL_A1, EARNED_GOLD)
                }
                connected(a)
                connected(b)
            }
        }
    }
}
