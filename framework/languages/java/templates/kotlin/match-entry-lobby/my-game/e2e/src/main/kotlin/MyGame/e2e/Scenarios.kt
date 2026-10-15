package MyGame.e2e

import MyGame.*
import java.util.UUID

private object Positions {
    const val NEAR_X = 10.0
    const val TARGET_X = 20.0
    const val WALL_BEHIND_X = 60.0
    const val FAR_X = 200.0
    const val LANE_Y = 10.0
}

private fun sameState(before: RoomView, after: RoomView) {
    check(before.copy(nodeId = after.nodeId) == after) { "Room state changed: $before -> $after" }
}

private suspend fun move(a: Client, b: Client, mover: Client, x: Double, sequence: Long) {
    mover.send(MoveRequest(x, Positions.LANE_Y, sequence))
    val moved: (RoomView) -> Boolean = { view ->
        view.players.any { it.playerId == mover.playerId && it.x == x && it.y == Positions.LANE_Y }
    }
    check(a.next(moved) == b.next<RoomView>(moved)) { "Movement views differ" }
}

private suspend fun hit(a: Client, b: Client, sequence: Long, hp: Int): RoomView {
    a.send(ShootRequest(b.playerId, sequence))
    val hit: (RoomView) -> Boolean = { view ->
        view.players.any { it.playerId == b.playerId && it.hp == hp }
    }
    val view = a.next(hit)
    check(view == b.next<RoomView>(hit)) { "Hit views differ" }
    return view
}

private suspend fun reject(
    a: Client,
    b: Client,
    sequence: Long,
    reason: String,
    target: String = b.playerId,
) {
    val before = b.view()
    a.send(ShootRequest(target, sequence))
    check(a.next<ShotRejected>().reason == reason) { "Wrong shot rejection" }
    sameState(before, b.view())
}

private suspend fun match(a: Client, b: Client): RoomView {
    a.queue()
    b.queue()
    check(a.next<MatchFound>().roomId == b.next<MatchFound>().roomId) {
        "Players matched different rooms"
    }
    val full: (RoomView) -> Boolean = { it.players.size == Game.PLAYERS_PER_ROOM }
    val room = a.next(full)
    check(room == b.next<RoomView>(full)) { "Initial room views differ" }
    return room
}

private suspend fun newMatch(endpoint: String, replacement: String) {
    val suffix = UUID.randomUUID().toString()
    val a = Client(endpoint, "new-a-$suffix")
    val b = Client(endpoint, "new-b-$suffix")
    try {
        a.connect()
        b.connect()
        check(match(a, b).nodeId == replacement) { "Replacement did not accept the new room" }
    } finally {
        try {
            b.close()
        } finally {
            a.close()
        }
    }
}

suspend fun scenario(id: String, nodes: Nodes) {
    val suffix = UUID.randomUUID().toString()
    val a = Client(nodes.endpoint, "p1-$suffix")
    val b = Client(nodes.endpoint, "p2-$suffix")
    var bClosed = false
    try {
        a.connect()
        b.connect()
        match(a, b)
        val session1 = a.sessionInfo()
        val session2 = b.sessionInfo()
        move(a, b, a, Positions.NEAR_X, 1)
        move(a, b, b, Positions.TARGET_X, 1)
        val before = hit(a, b, 2, Game.INITIAL_HP - Game.SHOT_DAMAGE)
        when (id) {
            "hit" -> {
                move(a, b, b, Positions.WALL_BEHIND_X, 2)
                reject(a, b, 3, Rejection.BLOCKED_BY_WALL)
                move(a, b, b, Positions.FAR_X, 3)
                reject(a, b, 4, Rejection.OUT_OF_RANGE)
                reject(a, b, 5, Rejection.NO_TARGET, "missing-player")
                move(a, b, b, Positions.TARGET_X, 4)
                for (sequence in 6L..8L) hit(
                    a,
                    b,
                    sequence,
                    Game.INITIAL_HP - Game.SHOT_DAMAGE * (sequence - 4).toInt(),
                )
                check(
                    a.next<MatchEnded>().winner == a.playerId &&
                        b.next<MatchEnded>().winner == a.playerId
                )
            }
            "reconnect" -> {
                bClosed = true
                b.close()
                val again = Client(nodes.endpoint, b.playerId)
                try {
                    again.connect()
                    again.queue()
                    val restored = again.next<RoomView>()
                    val identity = again.sessionInfo()
                    check(
                        identity.sessionId != session2.sessionId &&
                            identity.actorId == session2.actorId &&
                            identity.objectGeneration == session2.objectGeneration
                    ) {
                        "Reconnect changed Actor identity"
                    }
                    sameState(before, restored)
                    hit(a, again, 3, Game.INITIAL_HP - Game.SHOT_DAMAGE * 2)
                } finally {
                    again.close()
                }
            }
            "update" -> {
                nodes.drain(before.nodeId)
                check(a.sessionInfo() == session1 && b.sessionInfo() == session2) {
                    "Update changed binding"
                }
                sameState(before, a.view { it.nodeId != before.nodeId })
                sameState(before, b.view { it.nodeId != before.nodeId })
                nodes.replace(before.nodeId)
                newMatch(nodes.endpoint, before.nodeId)
                hit(a, b, 3, Game.INITIAL_HP - Game.SHOT_DAMAGE * 2)
                check(a.sessionInfo() == session1 && b.sessionInfo() == session2) {
                    "Replacement changed binding"
                }
            }
            else -> error("Unknown scenario: $id")
        }
        check(a.disconnects == 0 && a.connector.isConnected) { "First player disconnected" }
        if (!bClosed)
            check(b.disconnects == 0 && b.connector.isConnected) { "Second player disconnected" }
    } finally {
        try {
            if (!bClosed) b.close()
        } finally {
            a.close()
        }
    }
}
