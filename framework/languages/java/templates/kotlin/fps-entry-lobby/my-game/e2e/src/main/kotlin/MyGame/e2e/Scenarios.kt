package MyGame.e2e

import MyGame.*
import java.util.UUID
import kotlinx.coroutines.delay

object ScenarioList {
    val order = listOf("matchmaking", "hit", "wall", "input", "reconnect", "update")
}

private fun player(view: RoomView, client: Client) = view.players.single { it.playerId == client.playerId }

private fun nextSequence(view: RoomView, client: Client) = player(view, client).lastSequence + 1

private fun sameState(before: RoomView, after: RoomView) {
    check(before.copy(nodeId = after.nodeId) == after) { "Room state changed: $before -> $after" }
}

private suspend fun view(client: Client) = client.view { true }

private suspend fun matchmaking(nodes: Nodes) {
    val suffix = UUID.randomUUID().toString().replace("-", "")
    val rankedA = Client(nodes.endpoint, "ranked-a-$suffix")
    val casualA = Client(nodes.endpoint, "casual-a-$suffix")
    val rankedB = Client(nodes.endpoint, "ranked-b-$suffix")
    val casualB = Client(nodes.endpoint, "casual-b-$suffix")
    withCleanup(cleanup = { rankedA.close() }) {
        withCleanup(cleanup = { casualA.close() }) {
            withCleanup(cleanup = { rankedB.close() }) {
                withCleanup(cleanup = { casualB.close() }) {
                    rankedA.connect()
                    casualA.connect()
                    rankedB.connect()
                    casualB.connect()
                    rankedA.queue("ranked")
                    casualA.queue("casual")
                    rankedB.queue("ranked")
                    casualB.queue("casual")
                    val ranked = rankedA.next<MatchFound>()
                    val casual = casualA.next<MatchFound>()
                    check(ranked.roomId == rankedB.next<MatchFound>().roomId &&
                        casual.roomId == casualB.next<MatchFound>().roomId && ranked.roomId != casual.roomId) {
                        "Different modes were not separated."
                    }
                    val full: (RoomView) -> Boolean = { it.players.size == Game.PLAYERS_PER_ROOM }
                    val rankedView = rankedA.next(full)
                    val casualView = casualA.next(full)
                    check(rankedView.players.map { it.playerId }.toSet() == setOf(rankedA.playerId, rankedB.playerId) &&
                        casualView.players.map { it.playerId }.toSet() == setOf(casualA.playerId, casualB.playerId) &&
                        rankedView.phase == MatchPhase.Playing && casualView.phase == MatchPhase.Playing &&
                        rankedView.players.plus(casualView.players).all {
                            it.hp == Game.INITIAL_HP && it.lastSequence == 0L
                        }) { "Matched Room members or initial state differ." }
                    val rejected = rankedA.action(RoomAssignment(casual.roomId))
                    check(rejected is ActionRejected && rejected.reason == Rejection.PACKET_NOT_ALLOWED) {
                        "Session accepted an internal RoomAssignment."
                    }
                    sameState(rankedView, view(rankedA))
                    check(listOf(rankedA, rankedB, casualA, casualB).all { it.disconnects == 0 && it.connector.isConnected }) {
                        "Matchmaking disconnected a client."
                    }
                }
            }
        }
    }
}

suspend fun scenario(id: String, nodes: Nodes) {
    if (id == "matchmaking") {
        matchmaking(nodes)
        return
    }
    val suffix = UUID.randomUUID().toString().replace("-", "")
    val p1 = Client(nodes.endpoint, "p1-$suffix")
    val p2 = Client(nodes.endpoint, "p2-$suffix")
    withCleanup(cleanup = { p1.close() }) {
        withCleanup(cleanup = { p2.close() }) {
            p1.connect()
            p2.connect()
            p1.queue()
            p2.queue()
            val found1 = p1.next<MatchFound>()
            val found2 = p2.next<MatchFound>()
            check(found1.roomId == found2.roomId) { "Same-mode players entered different rooms." }
            val full: (RoomView) -> Boolean = { it.players.size == Game.PLAYERS_PER_ROOM }
            val ready1 = p1.next(full)
            val ready2 = p2.next(full)
            check(ready1 == ready2 && ready1.phase == MatchPhase.Playing &&
                ready1.players.all { it.hp == Game.INITIAL_HP && it.lastSequence == 0L }) {
                "Initial room state differs."
            }
            val session1 = p1.sessionInfo()
            val session2 = p2.sessionInfo()
            when (id) {
                "hit" -> hitScenario(p1, p2)
                "wall" -> wallScenario(p1, p2)
                "input" -> inputScenario(p1, p2)
                "reconnect" -> reconnectScenario(nodes.endpoint, p1, p2, session2)
                "update" -> updateScenario(nodes, p1, p2, session1, session2)
                else -> error("Unknown scenario: $id")
            }
            check(p1.disconnects == 0 && p1.connector.isConnected) { "First player disconnected." }
            if (id != "reconnect") check(p2.connector.isConnected) { "Second player disconnected." }
        }
    }
}

private suspend fun move(client: Client, delta: Vec3) {
    val before = view(client)
    val request = MoveRequest(delta, nextSequence(before, client))
    while (true) {
        when (val result = client.action(request)) {
            is ActionRejected -> {
                if (result.reason != Rejection.MOVE_BUDGET || result.retryAfterMs <= 0) {
                    error("Movement was rejected: $result; delta=$delta")
                }
                sameState(before, view(client))
                delay(result.retryAfterMs.toLong())
            }
            is MoveResult -> {
                check(result.sequence == request.sequence && result.position == player(before, client).position + delta) {
                    "Movement result differs: $result; delta=$delta; before=${player(before, client)}"
                }
                val after = view(client)
                check(player(after, client).lastSequence == request.sequence) { "Movement lost sequence." }
                check(after.players.all { it.position.y == 0.0 }) { "Movement changed feet height." }
                return
            }
            else -> error("Unexpected movement response: $result")
        }
    }
}

private suspend fun walk(client: Client, destination: Vec3) {
    var position = player(view(client), client).position
    while (position != destination) {
        val remaining = destination - position
        val delta = when {
            remaining.x != 0.0 -> Vec3(remaining.x.coerceIn(-Game.MAX_STEP, Game.MAX_STEP), 0.0, 0.0)
            remaining.y != 0.0 -> Vec3(0.0, remaining.y.coerceIn(-Game.MAX_STEP, Game.MAX_STEP), 0.0)
            else -> Vec3(0.0, 0.0, remaining.z.coerceIn(-Game.MAX_STEP, Game.MAX_STEP))
        }
        move(client, delta)
        position = player(view(client), client).position
    }
}

private suspend fun aim(shooter: Client, target: Client): Vec3 {
    val room = view(shooter)
    return player(room, target).position - player(room, shooter).position
}

private suspend fun shoot(shooter: Client, aim: Vec3, expectedOutcome: String) {
    val before = view(shooter)
    val request = ShootRequest(aim, nextSequence(before, shooter))
    while (true) {
        when (val result = shooter.action(request)) {
            is ShotRejected -> {
                if (result.reason != Rejection.FIRE_TOO_SOON || result.retryAfterMs <= 0) {
                    error("Shot was rejected: $result")
                }
                sameState(before, view(shooter))
                delay(result.retryAfterMs.toLong())
            }
            is ShotResult -> {
                check(result.outcome == expectedOutcome && result.sequence == request.sequence) {
                    "Expected $expectedOutcome, received $result."
                }
                return
            }
            else -> error("Unexpected shot response: $result")
        }
    }
}

private suspend fun hit(shooter: Client, target: Client, hp: Int) {
    shoot(shooter, aim(shooter, target), ShotOutcome.HIT)
    val a = view(shooter)
    val b = view(target)
    check(a == b && player(a, target).hp == hp) { "Hit views or damage differ." }
}

private suspend fun ended(winner: Client, other: Client) {
    check(winner.next<MatchEnded>().winner == winner.playerId &&
        other.next<MatchEnded>().winner == winner.playerId) { "Wrong match winner." }
    val room = view(winner)
    check(room.phase == MatchPhase.Finished && room.winner == winner.playerId) { "Finished state differs." }
}

private suspend fun rejected(client: Client, request: Any, reason: String) {
    val before = view(client)
    val result = client.action(request)
    val valid = when (result) {
        is ActionRejected -> result.retryAfterMs == 0 && result.reason == reason
        is ShotRejected -> result.retryAfterMs == 0 && result.reason == reason
        else -> false
    }
    check(valid) { "Expected rejection $reason, received $result." }
    sameState(before, view(client))
    println("input rejected: $reason; state unchanged")
}

private suspend fun hitScenario(a: Client, b: Client) {
    hit(a, b, Game.INITIAL_HP - Game.SHOT_DAMAGE)
    shoot(a, Vec3(0.0, 1.0, 0.0), ShotOutcome.MISS)
    check(player(view(b), b).hp == 75) { "An upward miss caused damage." }
    walk(a, Vec3(5.0, 0.0, 18.0))
    walk(a, Vec3(5.0, 0.0, 65.0))
    walk(b, Vec3(52.0, 0.0, 65.0))
    walk(b, Vec3(95.0, 0.0, 65.0))
    shoot(a, aim(a, b), ShotOutcome.MISS)
    check(player(view(b), b).hp == 75) { "An out-of-range ray caused damage." }
    walk(a, Vec3(20.0, 0.0, 65.0))
    for (hp in listOf(50, 25, 0)) hit(a, b, hp)
    ended(a, b)
    val room = view(a)
    rejected(a, MoveRequest(Vec3(0.0, 0.0, 1.0), nextSequence(room, a)), Rejection.MATCH_FINISHED)
    rejected(a, ShootRequest(Vec3(1.0, 0.0, 0.0), nextSequence(view(a), a)), Rejection.MATCH_FINISHED)
}

private suspend fun wallScenario(a: Client, b: Client) {
    walk(a, Vec3(48.0, 0.0, 22.0))
    walk(b, Vec3(52.0, 0.0, 22.0))
    val beforeWall = view(b)
    shoot(a, aim(a, b), ShotOutcome.BLOCKED_BY_WALL)
    check(player(view(b), b).hp == player(beforeWall, b).hp) { "A wall-blocked ray caused damage." }
    walk(a, ArenaMap.spawns[0])
    walk(b, ArenaMap.spawns[1])
    hit(a, b, 75)
    println("wall: intervening wall blocks; wall behind target permits hit")
}

private suspend fun inputScenario(a: Client, b: Client) {
    move(a, Vec3(0.0, 0.0, -1.0))
    val room = view(a)
    var sequence = player(room, a).lastSequence
    rejected(a, MoveRequest(Vec3(0.0, 0.0, 1.0), sequence), Rejection.STALE_SEQUENCE)
    rejected(a, ShootRequest(Vec3(1.0, 0.0, 0.0), sequence - 1), Rejection.STALE_SEQUENCE)
    rejected(a, MoveRequest(Vec3(Game.MAX_STEP + 1.0, 0.0, 0.0), sequence + 1), Rejection.STEP_TOO_LARGE)
    rejected(a, MoveRequest(Vec3(0.0, 1.0, 0.0), sequence + 1), Rejection.VERTICAL_MOVE)
    rejected(a, MoveRequest(Vec3(0.0, -1.0, 0.0), sequence + 1), Rejection.VERTICAL_MOVE)
    walk(a, Vec3(player(room, a).position.x, 0.0, 0.0))
    sequence = player(view(a), a).lastSequence
    rejected(a, MoveRequest(Vec3(0.0, 0.0, -1.0), sequence + 1), Rejection.OUTSIDE_MAP)
    rejected(a, ShootRequest(Vec3(), sequence + 1), Rejection.INVALID_AIM)
    walk(a, Vec3(48.0, 0.0, 22.0))
    sequence = nextSequence(view(a), a)
    rejected(a, MoveRequest(Vec3(2.0, 0.0, 0.0), sequence), Rejection.MOVE_BLOCKED_BY_WALL)
    move(a, Vec3(0.0, 0.0, -2.0))
    move(a, Vec3(0.0, 0.0, -2.0))
    move(a, Vec3(0.0, 0.0, -2.0))
    val beforeBudget = view(a)
    val request = MoveRequest(Vec3(0.0, 0.0, -2.0), nextSequence(beforeBudget, a))
    val budget = a.action(request)
    check(budget is ActionRejected && budget.reason == Rejection.MOVE_BUDGET && budget.retryAfterMs > 0) {
        "Expected movement-budget rejection."
    }
    sameState(beforeBudget, view(a))
    delay((budget as ActionRejected).retryAfterMs.toLong())
    check(a.action(request) is MoveResult) { "Server movement retry delay was insufficient." }
    shoot(a, Vec3(0.0, 1.0, 0.0), ShotOutcome.MISS)
    val beforeFire = view(a)
    val shot = ShootRequest(Vec3(0.0, 1.0, 0.0), nextSequence(beforeFire, a))
    val fire = a.action(shot)
    check(fire is ShotRejected && fire.reason == Rejection.FIRE_TOO_SOON && fire.retryAfterMs > 0) {
        "Expected fire-interval rejection."
    }
    sameState(beforeFire, view(a))
    delay((fire as ShotRejected).retryAfterMs.toLong())
    check(a.action(shot) is ShotResult) { "Server fire retry delay was insufficient." }
    check(player(view(b), b).hp == Game.INITIAL_HP) { "Input checks damaged target." }
    println("input: move-budget and fire-too-soon retry delays accepted unchanged requests")
}

private suspend fun reconnectScenario(
    endpoint: String,
    p1: Client,
    p2: Client,
    originalSession: SessionInfo,
) {
    walk(p2, Vec3(54.0, 0.0, 18.0))
    hit(p1, p2, 75)
    val beforeReconnect = view(p2)
    p2.close()
    val again = Client(endpoint, p2.playerId)
    withCleanup(cleanup = { again.close() }) {
        again.connect()
        again.queue()
        sameState(beforeReconnect, again.next<RoomView>())
        val identity = again.sessionInfo()
        check(identity.sessionId != originalSession.sessionId && identity.actorId == originalSession.actorId &&
            identity.objectGeneration == originalSession.objectGeneration) { "Reconnect changed Actor identity." }
        move(again, Vec3(0.0, 0.0, 1.0))
        for (hp in listOf(50, 25, 0)) hit(p1, again, hp)
        ended(p1, again)
        again.close()
    }
    val finished = Client(endpoint, p2.playerId)
    withCleanup(cleanup = { finished.close() }) {
        finished.connect()
        finished.queue()
        val restored = finished.next<RoomView>()
        check(restored.phase == MatchPhase.Finished && restored.winner == p1.playerId &&
            player(restored, finished).hp == 0) { "Finished reconnect lost winner or HP." }
    }
}

private suspend fun newMatch(nodes: Nodes, replacementId: String) {
    val suffix = UUID.randomUUID().toString().replace("-", "")
    val a = Client(nodes.endpoint, "new-a-$suffix")
    val b = Client(nodes.endpoint, "new-b-$suffix")
    withCleanup(cleanup = { a.close() }) {
        withCleanup(cleanup = { b.close() }) {
            a.connect()
            b.connect()
            a.queue()
            b.queue()
            check(a.next<MatchFound>().roomId == b.next<MatchFound>().roomId) { "Replacement matchmaking failed." }
            val full: (RoomView) -> Boolean = { it.players.size == Game.PLAYERS_PER_ROOM }
            val room = a.next(full)
            b.next(full)
            check(room.nodeId == replacementId) { "Replacement node did not accept the new room." }
        }
    }
}

private suspend fun updateScenario(
    nodes: Nodes,
    p1: Client,
    p2: Client,
    session1: SessionInfo,
    session2: SessionInfo,
) {
    move(p1, Vec3(0.0, 0.0, -1.0))
    hit(p1, p2, 75)
    val before = view(p1)
    nodes.drain(before.nodeId)
    check(p1.sessionInfo() == session1 && p2.sessionInfo() == session2) { "Update changed session binding." }
    sameState(before, p1.view { it.nodeId != before.nodeId })
    sameState(before, p2.view { it.nodeId != before.nodeId })
    nodes.replace(before.nodeId)
    newMatch(nodes, before.nodeId)
    hit(p1, p2, 50)
    check(p1.sessionInfo() == session1 && p2.sessionInfo() == session2) { "Replacement changed session binding." }
}
