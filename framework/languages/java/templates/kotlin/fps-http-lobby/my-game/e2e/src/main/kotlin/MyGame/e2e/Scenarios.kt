package MyGame.e2e

import MyGame.*
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Duration
import java.util.UUID
import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.delay

object ScenarioList {
    val order = listOf("lobby", "hit", "wall", "input", "reconnect", "update")
}

private fun player(view: RoomView, client: Client) = view.players.single { it.playerId == client.playerId }

private fun nextSequence(view: RoomView, client: Client) = player(view, client).lastSequence + 1

private fun sameState(before: RoomView, after: RoomView) {
    check(before.copy(nodeId = after.nodeId) == after) { "Room state changed: $before -> $after" }
}

private suspend fun view(client: Client) = client.view { true }

suspend fun scenario(id: String, nodes: Nodes) {
    if (id == "lobby") {
        lobbyScenario(nodes)
        return
    }
    val suffix = UUID.randomUUID().toString().replace("-", "")
    val playerA = "p1-$suffix"
    val playerB = "p2-$suffix"
    val assignment = matchPair(nodes, playerA, playerB)
    if (id == "update") rejectOutsider(nodes.endpoint, assignment.roomId)
    val p1 = Client(nodes.endpoint, playerA, assignment.roomId)
    val p2 = Client(nodes.endpoint, playerB, assignment.roomId)
    withCleanup(cleanup = { p1.close() }) {
        withCleanup(cleanup = { p2.close() }) {
            p1.connect()
            p2.connect()
            p1.join()
            p2.join()
            val found1 = p1.next<MatchFound>()
            val found2 = p2.next<MatchFound>()
            check(found1.roomId == found2.roomId && found1.roomId == assignment.roomId) {
                "Same-mode players entered a Room different from the HTTP assignment."
            }
            val full: (RoomView) -> Boolean = { it.players.size == Game.PLAYERS_PER_ROOM }
            val ready1 = p1.next<RoomView>(full)
            val ready2 = p2.next<RoomView>(full)
            check(ready1 == ready2 && ready1.roomId == assignment.roomId &&
                ready1.phase == MatchPhase.Playing &&
                ready1.players.all { it.hp == Game.INITIAL_HP && it.lastSequence == 0L }) {
                "Initial room state or HTTP assignment differs."
            }
            val session1 = p1.sessionInfo()
            val session2 = p2.sessionInfo()
            when (id) {
                "hit" -> hitScenario(p1, p2)
                "wall" -> wallScenario(p1, p2)
                "input" -> inputScenario(p1, p2)
                "reconnect" -> reconnectScenario(nodes.endpoint, assignment.roomId, p1, p2, session2)
                "update" -> updateScenario(nodes, p1, p2, session1, session2)
                else -> error("Unknown scenario: $id")
            }
            check(p1.disconnects.get() == 0 && p1.connector.isConnected) { "First player disconnected." }
            if (id != "reconnect") check(p2.connector.isConnected) { "Second player disconnected." }
        }
    }
}

private suspend fun lobbyScenario(nodes: Nodes) {
    val suffix = UUID.randomUUID().toString().replace("-", "")
    val defaultAId = "duel-a-$suffix"
    val defaultBId = "duel-b-$suffix"
    val rankedAId = "ranked-a-$suffix"
    val rankedBId = "ranked-b-$suffix"
    val defaultARequest = requestMatch(nodes.lobbyEndpoint, defaultAId, null)
    val rankedARequest = requestMatch(nodes.lobbyEndpoint, rankedAId, "ranked")
    val defaultBRequest = requestMatch(nodes.lobbyEndpoint, defaultBId, Names.DEFAULT_MODE)
    val rankedBRequest = requestMatch(nodes.lobbyEndpoint, rankedBId, "ranked")
    val duel = defaultARequest.join()
    val ranked = rankedARequest.join()
    check(
        duel == defaultBRequest.join() && ranked == rankedBRequest.join() &&
            duel.roomId != ranked.roomId && duel.sessionEndpoint == nodes.endpoint &&
            ranked.sessionEndpoint == nodes.endpoint,
    ) { "HTTP mode pairing or endpoint differs." }

    rejectOutsider(nodes.endpoint, duel.roomId)
    val a = Client(nodes.endpoint, defaultAId, duel.roomId)
    val b = Client(nodes.endpoint, defaultBId, duel.roomId)
    val c = Client(nodes.endpoint, rankedAId, ranked.roomId)
    val d = Client(nodes.endpoint, rankedBId, ranked.roomId)
    withCleanup(cleanup = { a.close() }) {
        withCleanup(cleanup = { b.close() }) {
            withCleanup(cleanup = { c.close() }) {
                withCleanup(cleanup = { d.close() }) {
                    a.connect()
                    b.connect()
                    c.connect()
                    d.connect()
                    val forged = PrepareRoom(duel.roomId, listOf("forged-a", "forged-b"))
                    val beforeJoin = a.action(forged)
                    check(beforeJoin is ActionRejected && beforeJoin.reason == Rejection.PACKET_NOT_ALLOWED) {
                        "Session accepted a client PrepareRoom before admission."
                    }
                    a.join()
                    b.join()
                    c.join()
                    d.join()
                    val full: (RoomView) -> Boolean = { it.players.size == Game.PLAYERS_PER_ROOM }
                    val duelView = a.next<RoomView>(full)
                    val rankedView = c.next<RoomView>(full)
                    check(
                        duelView.players.map { it.playerId }.toSet() == setOf(a.playerId, b.playerId) &&
                            rankedView.players.map { it.playerId }.toSet() == setOf(c.playerId, d.playerId),
                    ) { "Prepared Room roster differs from HTTP members." }
                    val duringMatch = a.action(forged)
                    check(duringMatch is ActionRejected && duringMatch.reason == Rejection.PACKET_NOT_ALLOWED) {
                        "Session accepted a client PrepareRoom during the match."
                    }
                    sameState(duelView, view(a))
                    move(a, Vec3(0.0, 0.0, -1.0))
                    hit(a, b, Game.INITIAL_HP - Game.SHOT_DAMAGE)
                    val beforeRestart = view(a)
                    val session = a.sessionInfo()
                    nodes.restartLobby()
                    sameState(beforeRestart, view(a))
                    check(a.sessionInfo() == session) { "Lobby restart changed an existing Session." }

                    val freshA = "restart-a-$suffix"
                    val freshB = "restart-b-$suffix"
                    val fresh = matchPair(nodes, freshA, freshB)
                    check(
                        fresh.roomId != duel.roomId && fresh.roomId != ranked.roomId &&
                            fresh.sessionEndpoint == nodes.endpoint,
                    ) { "Lobby restart reused an existing Room." }
                    val x = Client(nodes.endpoint, freshA, fresh.roomId)
                    val y = Client(nodes.endpoint, freshB, fresh.roomId)
                    withCleanup(cleanup = { x.close() }) {
                        withCleanup(cleanup = { y.close() }) {
                            x.connect()
                            y.connect()
                            x.join()
                            y.join()
                            val freshView = x.next<RoomView>(full)
                            y.next<RoomView>(full)
                            check(
                                freshView.roomId == fresh.roomId &&
                                    freshView.players.all { it.hp == Game.INITIAL_HP },
                            ) { "Restarted Lobby did not provision a new Room." }
                            sameState(beforeRestart, view(a))
                            check(listOf(a, b, c, d, x, y).all { it.connector.isConnected && it.disconnects.get() == 0 }) {
                                "Lobby flow disconnected a client."
                            }
                        }
                    }
                }
            }
        }
    }
    println("lobby: default duel, mode separation, roster, PrepareRoom rejection and restart verified")
}

private suspend fun rejectOutsider(endpoint: String, roomId: String) {
    val outsider = Client(endpoint, "outsider-${UUID.randomUUID().toString().replace("-", "")}", roomId)
    withCleanup(cleanup = { outsider.close() }) {
        outsider.connect()
        outsider.join()
        val rejected = outsider.next<ActionRejected>()
        check(rejected.reason == Rejection.ADMISSION_REJECTED) {
            "Room accepted a player absent from its prepared roster."
        }
    }
}

private fun matchPair(nodes: Nodes, firstId: String, secondId: String): MatchResponse {
    val first = requestMatch(nodes.lobbyEndpoint, firstId, null)
    val second = requestMatch(nodes.lobbyEndpoint, secondId, Names.DEFAULT_MODE)
    val assignment = first.join()
    check(assignment == second.join() && assignment.sessionEndpoint == nodes.endpoint) {
        "HTTP players received different Room or SessionEndpoint."
    }
    return assignment
}

private fun requestMatch(endpoint: String, playerId: String, mode: String?): CompletableFuture<MatchResponse> {
    val body =
        if (mode == null) Json.mapper.writeValueAsBytes(mapOf("playerId" to playerId))
        else Json.mapper.writeValueAsBytes(MatchRequest(playerId, mode))
    val request =
        HttpRequest.newBuilder(URI.create(endpoint + LobbyNames.MATCH_PATH))
            .timeout(Duration.ofSeconds(MESSAGE_SECONDS))
            .header(HttpProtocol.CONTENT_TYPE, HttpProtocol.JSON)
            .POST(HttpRequest.BodyPublishers.ofByteArray(body))
            .build()
    return HttpClient.newHttpClient()
        .sendAsync(request, HttpResponse.BodyHandlers.ofByteArray())
        .thenApply { response ->
            check(response.statusCode() == HttpProtocol.OK) { "HTTP status ${response.statusCode()}" }
            Json.mapper.readValue(response.body(), MatchResponse::class.java)
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
    val beforeBudget = view(a)
    val moveSequence = nextSequence(beforeBudget, a)
    val acceptedMoveRequest = MoveRequest(Vec3(0.0, 0.0, -2.0), moveSequence)
    val request = MoveRequest(Vec3(0.0, 0.0, -2.0), moveSequence + 1)
    a.send(acceptedMoveRequest)
    a.send(request)
    val acceptedMove = a.next<MoveResult>()
    check(acceptedMove.sequence == moveSequence) {
        "Expected accepted movement before movement-budget rejection."
    }
    val budget = a.next<ActionRejected>()
    check(budget.reason == Rejection.MOVE_BUDGET && budget.retryAfterMs > 0) {
        "Expected movement-budget rejection."
    }
    val afterBudget = view(a)
    val beforeBudgetPlayer = player(beforeBudget, a)
    val afterBudgetPlayer = player(afterBudget, a)
    check(afterBudgetPlayer.position == acceptedMove.position && afterBudgetPlayer.lastSequence == moveSequence) {
        "Movement-budget rejection changed the accepted movement position or sequence."
    }
    check(beforeBudgetPlayer.hp == afterBudgetPlayer.hp && player(beforeBudget, b).hp == player(afterBudget, b).hp) {
        "Movement-budget rejection changed player HP."
    }
    delay(budget.retryAfterMs.toLong())
    val retriedMove = a.action(request)
    check(retriedMove is MoveResult && retriedMove.sequence == request.sequence) {
        "Server movement retry delay was insufficient."
    }

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
    val beforeFire = view(a)
    val shotSequence = nextSequence(beforeFire, a)
    val acceptedShotRequest = ShootRequest(Vec3(0.0, 1.0, 0.0), shotSequence)
    val shot = ShootRequest(Vec3(0.0, 1.0, 0.0), shotSequence + 1)
    a.send(acceptedShotRequest)
    a.send(shot)
    val acceptedShot = a.next<ShotResult>()
    check(acceptedShot.sequence == shotSequence && acceptedShot.outcome == ShotOutcome.MISS) {
        "Expected accepted miss before fire-too-soon rejection."
    }
    val fire = a.next<ShotRejected>()
    check(fire.reason == Rejection.FIRE_TOO_SOON && fire.retryAfterMs > 0) {
        "Expected fire-interval rejection."
    }
    val afterFire = view(a)
    val beforeFirePlayer = player(beforeFire, a)
    val afterFirePlayer = player(afterFire, a)
    check(afterFirePlayer.position == beforeFirePlayer.position && afterFirePlayer.lastSequence == shotSequence) {
        "Fire-too-soon rejection changed the accepted shot position or sequence."
    }
    check(beforeFirePlayer.hp == afterFirePlayer.hp && player(beforeFire, b).hp == player(afterFire, b).hp) {
        "Fire-too-soon rejection changed player HP."
    }
    delay(fire.retryAfterMs.toLong())
    val retriedShot = a.action(shot)
    check(retriedShot is ShotResult
            && retriedShot.sequence == shot.sequence
            && retriedShot.outcome == ShotOutcome.MISS) { "Server fire retry delay was insufficient." }
    check(player(view(b), b).hp == Game.INITIAL_HP) { "Input checks damaged target." }
    println("input: move-budget and fire-too-soon retry delays accepted unchanged requests")
}

private suspend fun reconnectScenario(
    endpoint: String,
    roomId: String,
    p1: Client,
    p2: Client,
    originalSession: SessionInfo,
) {
    walk(p2, Vec3(54.0, 0.0, 18.0))
    hit(p1, p2, 75)
    val beforeReconnect = view(p2)
    p2.close()
    val again = Client(endpoint, p2.playerId, roomId)
    withCleanup(cleanup = { again.close() }) {
        again.connect()
        again.join()
        sameState(beforeReconnect, again.next<RoomView>())
        val identity = again.sessionInfo()
        check(identity.sessionId != originalSession.sessionId && identity.actorId == originalSession.actorId &&
            identity.objectGeneration == originalSession.objectGeneration) { "Reconnect changed Actor identity." }
        move(again, Vec3(0.0, 0.0, 1.0))
        for (hp in listOf(50, 25, 0)) hit(p1, again, hp)
        ended(p1, again)
        again.close()
    }
    val finished = Client(endpoint, p2.playerId, roomId)
    withCleanup(cleanup = { finished.close() }) {
        finished.connect()
        finished.join()
        val restored = finished.next<RoomView>()
        check(restored.phase == MatchPhase.Finished && restored.winner == p1.playerId &&
            player(restored, finished).hp == 0) { "Finished reconnect lost winner or HP." }
    }
}

private suspend fun newMatch(nodes: Nodes, replacementId: String) {
    val suffix = UUID.randomUUID().toString().replace("-", "")
    val firstId = "new-a-$suffix"
    val secondId = "new-b-$suffix"
    val assignment = matchPair(nodes, firstId, secondId)
    val a = Client(nodes.endpoint, firstId, assignment.roomId)
    val b = Client(nodes.endpoint, secondId, assignment.roomId)
    withCleanup(cleanup = { a.close() }) {
        withCleanup(cleanup = { b.close() }) {
            a.connect()
            b.connect()
            a.join()
            b.join()
            val full: (RoomView) -> Boolean = { it.players.size == Game.PLAYERS_PER_ROOM }
            val room = a.next<RoomView>(full)
            b.next<RoomView>(full)
            check(room.roomId == assignment.roomId) { "Replacement matchmaking failed." }
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
