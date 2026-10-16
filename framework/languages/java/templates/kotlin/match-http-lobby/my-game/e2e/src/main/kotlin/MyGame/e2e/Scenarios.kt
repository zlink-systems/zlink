package MyGame.e2e

import MyGame.*
import java.net.URI
import java.net.http.*
import java.time.Duration
import java.util.UUID
import systems.zlink.framework.kotlin.await

private const val NEAR_X = 10.0
private const val TARGET_X = 20.0
private const val BEHIND_WALL_X = 60.0
private const val FAR_X = 200.0
private const val LANE_Y = 10.0

suspend fun scenario(id: String, nodes: Nodes) {
    val suffix = UUID.randomUUID().toString()
    val assignment = match(nodes, "p1-$suffix", "p2-$suffix")
    val a = Client(assignment.sessionEndpoint, "p1-$suffix", assignment.roomId)
    val b = Client(assignment.sessionEndpoint, "p2-$suffix", assignment.roomId)
    try {
        a.connect()
        b.connect()
        val initial = both(a, b) { it.players.size == Rules.PLAYERS }
        check(
            initial.roomId == assignment.roomId && initial.players.all { it.hp == Rules.INITIAL_HP }
        )
        if (Scenario.parse(id) == Scenario.LOBBY) {
            connected(a, b)
            return
        }
        val identityA = a.sessionInfo()
        val identityB = b.sessionInfo()
        move(a, b, a, NEAR_X, LANE_Y, 1)
        move(a, b, b, TARGET_X, LANE_Y, 1)
        val before = hit(a, b, 2, Rules.INITIAL_HP - Rules.DAMAGE)
        when (Scenario.parse(id)) {
            Scenario.HIT -> {
                move(a, b, b, BEHIND_WALL_X, LANE_Y, 2)
                reject(a, b, 3, Rules.BLOCKED, b.playerId)
                move(a, b, b, FAR_X, LANE_Y, 3)
                reject(a, b, 4, Rules.OUT_OF_RANGE, b.playerId)
                reject(a, b, 5, Rules.NO_TARGET, "missing-player")
                move(a, b, b, TARGET_X, LANE_Y, 4)
                for (sequence in 6L..8L) hit(
                    a,
                    b,
                    sequence,
                    Rules.INITIAL_HP - Rules.DAMAGE * (sequence - 4).toInt(),
                )
                check(
                    a.next<MatchEnded>().winner == a.playerId &&
                        b.next<MatchEnded>().winner == a.playerId
                )
            }
            Scenario.RECONNECT -> {
                b.close()
                val again = Client(nodes.endpoint, b.playerId, assignment.roomId)
                try {
                    again.connect()
                    sameState(before, again.next<RoomView>())
                    val identity = again.sessionInfo()
                    check(
                        identity.sessionId != identityB.sessionId &&
                            identity.actorId == identityB.actorId &&
                            identity.objectGeneration == identityB.objectGeneration
                    )
                    a.clear()
                    again.clear()
                    hit(a, again, 3, Rules.INITIAL_HP - Rules.DAMAGE * 2)
                    connected(a, again)
                } finally {
                    again.close()
                }
                return
            }
            Scenario.UPDATE -> {
                nodes.drain(before.nodeId)
                check(a.sessionInfo() == identityA && b.sessionInfo() == identityB) {
                    "Session binding changed"
                }
                sameState(before, view(a))
                sameState(before, view(b))
                check(view(a).nodeId != before.nodeId) { "Room did not relocate" }
                nodes.replace(before.nodeId)
                newMatch(nodes)
                a.clear()
                b.clear()
                hit(a, b, 3, Rules.INITIAL_HP - Rules.DAMAGE * 2)
                check(a.sessionInfo() == identityA && b.sessionInfo() == identityB)
            }
            Scenario.LOBBY -> error("Unexpected scenario")
        }
        connected(a, b)
    } finally {
        try {
            b.close()
        } finally {
            a.close()
        }
    }
}

private suspend fun match(nodes: Nodes, a: String, b: String): MatchResponse =
    HttpClient.newHttpClient().use { http ->
        val first = post(http, nodes.lobbyEndpoint, a)
        val second = post(http, nodes.lobbyEndpoint, b)
        val assignment = first.await()
        check(assignment == second.await() && assignment.sessionEndpoint == nodes.endpoint) {
            "HTTP assignment mismatch"
        }
        assignment
    }

private fun post(http: HttpClient, endpoint: String, player: String) =
    http
        .sendAsync(
            HttpRequest.newBuilder(URI.create(endpoint + Names.MATCH_PATH))
                .timeout(Duration.ofSeconds(MESSAGE_SECONDS))
                .header(HttpProtocol.CONTENT_TYPE, HttpProtocol.JSON)
                .POST(
                    HttpRequest.BodyPublishers.ofByteArray(
                        Json.mapper.writeValueAsBytes(MatchRequest(player, Names.DEFAULT_MODE))
                    )
                )
                .build(),
            HttpResponse.BodyHandlers.ofByteArray(),
        )
        .thenApply { response ->
            check(response.statusCode() == HttpProtocol.OK) {
                "HTTP status ${response.statusCode()}"
            }
            Json.mapper.readValue(response.body(), MatchResponse::class.java)
        }

private suspend fun newMatch(nodes: Nodes) {
    val suffix = UUID.randomUUID().toString()
    val assignment = match(nodes, "new-a-$suffix", "new-b-$suffix")
    val a = Client(assignment.sessionEndpoint, "new-a-$suffix", assignment.roomId)
    val b = Client(assignment.sessionEndpoint, "new-b-$suffix", assignment.roomId)
    try {
        a.connect()
        b.connect()
        val room = both(a, b) { it.players.size == Rules.PLAYERS }
        check(room.roomId == assignment.roomId && nodes.isLiveMatch(room.nodeId)) {
            "New Room is not on a live match node"
        }
        connected(a, b)
    } finally {
        try {
            b.close()
        } finally {
            a.close()
        }
    }
}

private fun both(a: Client, b: Client, predicate: (RoomView) -> Boolean): RoomView {
    val view = a.next(predicate)
    check(view == b.next(predicate)) { "Room views differ" }
    return view
}

private suspend fun move(
    a: Client,
    b: Client,
    mover: Client,
    x: Double,
    y: Double,
    sequence: Long,
) {
    a.clear()
    b.clear()
    mover.send(MoveRequest(x, y, sequence))
    both(a, b) { v -> v.players.any { it.playerId == mover.playerId && it.x == x && it.y == y } }
}

private suspend fun hit(a: Client, b: Client, sequence: Long, hp: Int): RoomView {
    a.send(ShootRequest(b.playerId, sequence))
    return both(a, b) { v -> v.players.any { it.playerId == b.playerId && it.hp == hp } }
}

private suspend fun view(a: Client): RoomView {
    a.clear()
    a.send(RoomViewRequest())
    return a.next()
}

private suspend fun reject(a: Client, b: Client, sequence: Long, reason: String, target: String) {
    val before = view(b)
    a.send(ShootRequest(target, sequence))
    check(a.next<ShotRejected>().reason == reason) { "Wrong shot rejection" }
    sameState(before, view(b))
}

private fun sameState(a: RoomView, b: RoomView) {
    check(a.roomId == b.roomId && a.players == b.players) { "Room state changed" }
}

private fun connected(a: Client, b: Client) {
    check(
        a.connector.isConnected &&
            b.connector.isConnected &&
            a.disconnects.get() == 0 &&
            b.disconnects.get() == 0
    ) {
        "Client disconnected"
    }
}
