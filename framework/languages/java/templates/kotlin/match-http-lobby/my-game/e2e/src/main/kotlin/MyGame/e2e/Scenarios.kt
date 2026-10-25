package MyGame.e2e

import MyGame.*
import java.net.URI
import java.net.http.*
import java.time.Duration
import java.util.UUID
import kotlinx.coroutines.delay
import kotlin.math.hypot
import systems.zlink.framework.kotlin.await

private const val WALL_CENTER_Y = (Rules.WALL_MIN_Y + Rules.WALL_MAX_Y) / 2

suspend fun scenario(id: String, nodes: Nodes) {
    val suffix = UUID.randomUUID().toString()
    val playerA = "p1-$suffix"
    val playerB = "p2-$suffix"
    val assignment = match(nodes, playerA, playerB)
    check(Regex("${Names.ROOM_PREFIX}[0-9a-f]{32}").matches(assignment.roomId)) {
        "Lobby returned an invalid Room ID"
    }
    check(assignment.sessionEndpoint == nodes.endpoint) { "Lobby returned another Session endpoint" }

    val a = Client(assignment.sessionEndpoint, playerA, assignment.roomId)
    val b = Client(assignment.sessionEndpoint, playerB, assignment.roomId)
    withCleanup(cleanup = { withCleanup(cleanup = { a.close() }) { b.close() } }) {
        a.connect()
        b.connect()
        val initial = both(a, b) { it.players.size == Rules.PLAYERS }
        check(initial.roomId == assignment.roomId && initial.players.all { it.hp == Rules.INITIAL_HP })
        check(initial.phase == Rules.PLAYING && initial.winner == null) { "Initial phase differs" }
        check(initial.players.all { it.lastSequence == 0L }) { "Initial action sequence differs" }
        check(
            initial.players.map { it.x to it.y }.toSet() ==
                setOf(Rules.SPAWN_AX to Rules.SPAWN_AY, Rules.SPAWN_BX to Rules.SPAWN_BY) &&
                initial.players.any { it.x < Rules.WALL_X } &&
                initial.players.any { it.x > Rules.WALL_X }
        ) { "Server-selected spawn positions differ" }
        check(distance(initial.players[0], initial.players[1]) <= Rules.RANGE) {
            "Players spawned outside shot range"
        }

        when (Scenario.parse(id)) {
            Scenario.LOBBY -> {
                verifyOutsider(b, initial, assignment, suffix)
                verifyClientAllowlist(a, b, assignment.roomId)
                connected(a, b)
                verifyLobbyRestart(nodes, assignment, suffix)
                return
            }
            Scenario.HIT -> playHit(a, b, initial)
            Scenario.WALL -> playWall(a, b)
            Scenario.INPUT -> playInput(a, b)
            Scenario.RECONNECT -> {
                playReconnect(a, b, initial, assignment.sessionEndpoint)
                return
            }
            Scenario.UPDATE -> playUpdate(a, b, nodes, assignment.roomId)
        }
        connected(a, b)
    }
}

private suspend fun verifyOutsider(
    b: Client,
    initial: RoomView,
    assignment: MatchResponse,
    suffix: String,
) {
    verifyOutsiderState(b, initial, assignment.sessionEndpoint, "outsider-$suffix")
}

private suspend fun verifyOutsiderState(
    observer: Client,
    expected: RoomView,
    endpoint: String,
    outsiderId: String,
) {
    val outsider = Client(endpoint, outsiderId, expected.roomId)
    withCleanup(cleanup = { outsider.close() }) {
        outsider.connect()
        val rejection = outsider.next<ActionRejected> { it.reason == Rules.ADMISSION_REJECTED }
        check(rejection.retryAfterMs == 0) { "Admission rejection returned a retry duration" }
        sameState(expected, observer.view())
    }
}

private suspend fun verifyClientAllowlist(a: Client, observer: Client, roomId: String) {
    val before = observer.view()
    a.clear()
    a.send(PrepareRoom(roomId, listOf(a.playerId, observer.playerId)))
    val rejection = a.next<ActionRejected> { it.reason == Rules.PACKET_NOT_ALLOWED }
    check(rejection.retryAfterMs == 0) { "Internal packet rejection returned a retry duration" }
    sameState(before, observer.view())
}

private suspend fun verifyLobbyRestart(
    nodes: Nodes,
    previous: MatchResponse,
    suffix: String,
) {
    nodes.restartLobby()
    val firstId = "restart-a-$suffix"
    val secondId = "restart-b-$suffix"
    val next = match(nodes, firstId, secondId)
    check(next.roomId != previous.roomId) { "Lobby restart reused a Room ID" }
    verifyNewRoom(next, firstId, secondId, null)
}

private suspend fun playHit(a: Client, b: Client, initial: RoomView) {
    val bSpawn = initial.players.single { it.playerId == b.playerId }
    hit(a, b, 1, Rules.INITIAL_HP - Rules.DAMAGE)
    moveTo(a, b, b, Rules.WALL_X - 1, Rules.MAP_MAX)
    rejectShot(a, b, 2, Rules.OUT_OF_RANGE)
    rejectShot(a, b, 2, Rules.NO_TARGET, "missing-player")
    moveTo(a, b, b, bSpawn.x, bSpawn.y)

    for (sequence in 2L..4L) {
        hit(a, b, sequence, Rules.INITIAL_HP - Rules.DAMAGE * sequence.toInt())
    }
    check(a.next<MatchEnded>().winner == a.playerId && b.next<MatchEnded>().winner == a.playerId) {
        "Wrong match winner"
    }
    val final = b.view()
    check(final.phase == Rules.FINISHED_PHASE && final.winner == a.playerId) {
        "RoomView omitted the winner or finished phase"
    }
    check(final.players.single { it.playerId == a.playerId }.lastSequence == 4L) {
        "RoomView omitted the accepted action sequence"
    }
}

private suspend fun playInput(a: Client, b: Client) {
    val beforeMove = a.view()
    val firstSpawn = beforeMove.players.single {
        it.x == Rules.SPAWN_AX && it.y == Rules.SPAWN_AY
    }
    val mover = if (firstSpawn.playerId == a.playerId) a else b
    val other = if (mover === a) b else a
    val source = player(beforeMove, mover)
    val target = player(beforeMove, other)
    val moveDelta = if (target.x >= source.x) Rules.MAX_STEP else -Rules.MAX_STEP
    val moveSequence = source.lastSequence + 1
    val acceptedMove = MoveRequest(moveDelta, 0.0, moveSequence)
    val rejectedMove = MoveRequest(moveDelta, 0.0, moveSequence + 1)
    mover.send(acceptedMove)
    mover.send(rejectedMove)
    val moveResult = mover.next<MoveResult>()
    val moveRejection = mover.next<ActionRejected>()
    check(moveResult.sequence == moveSequence) { "Accepted move result sequence differs." }
    check(moveRejection.reason == Rules.MOVE_BUDGET && moveRejection.retryAfterMs > 0) {
        "Expected movement-budget rejection."
    }
    val afterMoveA = mover.view()
    val afterMover = player(afterMoveA, mover)
    check(afterMover.x == moveResult.x && afterMover.y == moveResult.y &&
        afterMover.lastSequence == moveSequence && player(afterMoveA, other).hp == target.hp) {
        "Movement-budget rejection changed the accepted movement state."
    }
    delay(moveRejection.retryAfterMs.toLong())
    mover.send(rejectedMove)
    val retriedMove = mover.next<MoveResult>()
    check(retriedMove.sequence == rejectedMove.sequence) { "Server movement retry delay was insufficient." }
    val afterMoveB = mover.view()
    val afterRetryMove = player(afterMoveB, mover)
    check(afterRetryMove.lastSequence == rejectedMove.sequence &&
        afterRetryMove.x == retriedMove.x && afterRetryMove.y == retriedMove.y) {
        "Retried movement result differs from RoomView."
    }

    val beforeShot = a.view()
    val shotTarget = player(beforeShot, b)
    val shotSequence = player(beforeShot, a).lastSequence + 1
    val acceptedShot = ShootRequest(b.playerId, shotSequence)
    val rejectedShot = ShootRequest(b.playerId, shotSequence + 1)
    a.send(acceptedShot)
    a.send(rejectedShot)
    val shotResult = a.next<ShotResult>()
    val fireRejection = a.next<ActionRejected>()
    check(shotResult.sequence == shotSequence && shotResult.outcome == "hit") {
        "Expected accepted hit before fire-too-soon rejection."
    }
    check(fireRejection.reason == Rules.FIRE_TOO_SOON && fireRejection.retryAfterMs > 0) {
        "Expected fire-interval rejection."
    }
    val afterShotA = a.view()
    check(player(afterShotA, b).hp == shotTarget.hp - Rules.DAMAGE &&
        player(afterShotA, a).lastSequence == shotSequence) {
        "Fire-too-soon rejection changed the accepted shot state."
    }
    delay(fireRejection.retryAfterMs.toLong())
    a.send(rejectedShot)
    val retriedShot = a.next<ShotResult>()
    check(retriedShot.sequence == rejectedShot.sequence) { "Server fire retry delay was insufficient." }
    val afterShotB = a.view()
    check(player(afterShotB, b).hp == shotTarget.hp - 2 * Rules.DAMAGE &&
        player(afterShotB, a).lastSequence == rejectedShot.sequence) {
        "Retried shot did not update the target state."
    }

    val staleSequence = player(mover.view(), mover).lastSequence
    rejectAction(mover, MoveRequest(0.0, 0.0, staleSequence), Rules.STALE_SEQUENCE)
    rejectAction(mover, MoveRequest(0.0, 0.0, moveSequence), Rules.STALE_SEQUENCE)
    val stepTooLargeSequence = staleSequence + 1
    rejectAction(
        mover,
        MoveRequest(Rules.MAX_STEP + 0.01, 0.0, stepTooLargeSequence),
        Rules.STEP_TOO_LARGE,
    )
    rejectAction(
        mover,
        MoveRequest(0.0, 0.0, Rules.MAX_SAFE_INTEGER + 1),
        Rules.INTEGER_OUT_OF_RANGE,
    )

    val outsideStart = moveTo(a, b, mover, 1.0, Rules.SPAWN_AY)
    val outsideSequence = player(outsideStart, mover).lastSequence + 1
    rejectAction(
        mover,
        MoveRequest(-Rules.MAX_STEP, 0.0, outsideSequence),
        Rules.OUTSIDE_MAP,
    )
    val beforeWall = moveTo(a, b, mover, Rules.WALL_X - Rules.MAX_STEP, WALL_CENTER_Y)
    val wallSequence = player(beforeWall, mover).lastSequence + 1
    rejectAction(mover, MoveRequest(Rules.MAX_STEP, 0.0, wallSequence), Rules.MOVE_BLOCKED_BY_WALL)
    move(a, b, mover, -0.1, 0.0, Rules.MAX_SAFE_INTEGER)
    check(player(mover.view(), mover).lastSequence == Rules.MAX_SAFE_INTEGER) {
        "Maximum safe sequence was not accepted."
    }
    rejectAction(
        mover,
        MoveRequest(0.0, 0.0, Rules.MAX_SAFE_INTEGER + 1),
        Rules.INTEGER_OUT_OF_RANGE,
    )
}

private suspend fun playReconnect(
    a: Client,
    b: Client,
    initial: RoomView,
    endpoint: String,
) {
    val previousIdentity = b.sessionInfo()
    var before = initial
    for (sequence in 1L..4L) {
        before = hit(a, b, sequence, Rules.INITIAL_HP - Rules.DAMAGE * sequence.toInt())
    }
    b.close()
    val again = Client(endpoint, b.playerId, before.roomId)
    withCleanup(cleanup = { again.close() }) {
        again.connect()
        val restored = again.next<RoomView> { it.phase == Rules.FINISHED_PHASE }
        val identity = again.sessionInfo()
        check(
            identity.sessionId != previousIdentity.sessionId &&
                identity.actorId == previousIdentity.actorId &&
                identity.objectGeneration == previousIdentity.objectGeneration
        ) { "Reconnect changed Actor identity" }
        sameState(before, restored)
        check(
            restored.winner == a.playerId &&
                restored.phase == Rules.FINISHED_PHASE &&
                restored.players.single { it.playerId == a.playerId }.lastSequence == 4L
        ) { "Reconnect did not restore sequence, phase, and winner" }
        connected(a, again)
    }
}

private suspend fun playUpdate(a: Client, b: Client, nodes: Nodes, previousRoomId: String) {
    val identityA = a.sessionInfo()
    val identityB = b.sessionInfo()
    val moved = move(a, b, a, 1.0, 0.0, 1)
    val before = hit(a, b, 2, Rules.INITIAL_HP - Rules.DAMAGE)
    check(before.players.single { it.playerId == a.playerId }.lastSequence == 2L) {
        "Update snapshot omitted the accepted action sequence"
    }
    check(moved.phase == Rules.PLAYING && moved.winner == null) { "Update input changed match phase" }
    nodes.drain(before.nodeId)
    check(a.sessionInfo() == identityA && b.sessionInfo() == identityB) { "Session binding changed" }
    val afterA = a.view() { it.nodeId != before.nodeId }
    val afterB = b.view() { it.nodeId != before.nodeId }
    sameState(before, afterA)
    sameState(before, afterB)
    verifyOutsiderState(a, afterA, nodes.endpoint, "outsider-update-${UUID.randomUUID()}")
    nodes.replace(before.nodeId)
    val suffix = UUID.randomUUID().toString()
    val firstId = "new-a-$suffix"
    val secondId = "new-b-$suffix"
    val assignment = match(nodes, firstId, secondId)
    check(assignment.roomId != previousRoomId) { "Matchmaker reused a Room ID" }
    verifyNewRoom(assignment, firstId, secondId, before.nodeId)
    hit(a, b, 3, Rules.INITIAL_HP - Rules.DAMAGE * 2)
    check(a.sessionInfo() == identityA && b.sessionInfo() == identityB) {
        "Replacement changed Session binding"
    }
}

private suspend fun verifyNewRoom(
    assignment: MatchResponse,
    playerA: String,
    playerB: String,
    expectedNodeId: String?,
) {
    val a = Client(assignment.sessionEndpoint, playerA, assignment.roomId)
    val b = Client(assignment.sessionEndpoint, playerB, assignment.roomId)
    withCleanup(cleanup = { withCleanup(cleanup = { a.close() }) { b.close() } }) {
        a.connect()
        b.connect()
        val room = both(a, b) { it.players.size == Rules.PLAYERS }
        check(room.roomId == assignment.roomId && room.phase == Rules.PLAYING && room.winner == null)
        if (expectedNodeId != null) check(room.nodeId == expectedNodeId) {
            "New Room is not on the replacement match node"
        }
    }
}

private suspend fun match(nodes: Nodes, a: String, b: String): MatchResponse =
    HttpClient.newHttpClient().use { http ->
        val first = post(http, nodes.lobbyEndpoint, a, null)
        val second = post(http, nodes.lobbyEndpoint, b, Names.DEFAULT_MODE)
        val assignment = first.await()
        check(assignment == second.await() && assignment.sessionEndpoint == nodes.endpoint) {
            "HTTP assignment mismatch"
        }
        assignment
    }

private fun post(http: HttpClient, endpoint: String, player: String, mode: String?) =
    http.sendAsync(
        HttpRequest.newBuilder(URI.create(endpoint + Names.MATCH_PATH))
            .timeout(Duration.ofSeconds(MESSAGE_SECONDS))
            .header(HttpProtocol.CONTENT_TYPE, HttpProtocol.JSON)
            .POST(
                HttpRequest.BodyPublishers.ofByteArray(
                    Json.mapper.writeValueAsBytes(
                        if (mode == null) mapOf("playerId" to player)
                        else mapOf("playerId" to player, "mode" to mode)
                    )
                )
            )
            .build(),
        HttpResponse.BodyHandlers.ofByteArray(),
    ).thenApply { response ->
        check(response.statusCode() == HttpProtocol.OK) { "HTTP status ${response.statusCode()}" }
        Json.mapper.readValue(response.body(), MatchResponse::class.java)
    }

private suspend fun moveTo(a: Client, b: Client, mover: Client, targetX: Double, targetY: Double): RoomView {
    var room = mover.view()
    while (true) {
        val player = room.players.single { it.playerId == mover.playerId }
        if (player.x == targetX && player.y == targetY) return room
        val dx =
            if (player.x == targetX) 0.0
            else Math.copySign(minOf(Rules.MAX_STEP, kotlin.math.abs(targetX - player.x)), targetX - player.x)
        val dy =
            if (player.x != targetX) 0.0
            else Math.copySign(minOf(Rules.MAX_STEP, kotlin.math.abs(targetY - player.y)), targetY - player.y)
        room = move(a, b, mover, dx, dy, player.lastSequence + 1)
    }
}

private suspend fun move(
    a: Client,
    b: Client,
    mover: Client,
    dx: Double,
    dy: Double,
    sequence: Long,
): RoomView {
    val observer = if (mover === a) b else a
    while (true) {
        val before = observer.view()
        val current = before.players.single { it.playerId == mover.playerId }
        val expectedX = current.x + dx
        val expectedY = current.y + dy
        fun moved(candidate: RoomView): Boolean {
            val player = candidate.players.single { it.playerId == mover.playerId }
            return player.lastSequence == sequence && player.x == expectedX && player.y == expectedY
        }
        mover.clear()
        mover.send(MoveRequest(dx, dy, sequence))
        val result = mover.next<Any> { it is ActionRejected || it is RoomView && moved(it) }
        if (result is RoomView) {
            val observed = observer.next<RoomView> { moved(it) }
            check(result == observed) { "Movement views differ" }
            return result
        }
        val rejection = result as ActionRejected
        sameState(before, observer.view())
        if (rejection.reason != Rules.MOVE_BUDGET)
            error("Movement rejected: ${rejection.reason}")
        check(rejection.retryAfterMs > 0) { "Move budget rejection omitted RetryAfterMs" }
        delay(rejection.retryAfterMs.toLong())
    }
}

private suspend fun hit(a: Client, b: Client, sequence: Long, hp: Int): RoomView {
    fun hitObserved(view: RoomView): Boolean =
        view.players.any { it.playerId == b.playerId && it.hp == hp } &&
            view.players.single { it.playerId == a.playerId }.lastSequence == sequence

    while (true) {
        val before = b.view()
        a.clear()
        a.send(ShootRequest(b.playerId, sequence))
        val result = a.next<Any> { it is ActionRejected || it is RoomView && hitObserved(it) }
        if (result is RoomView) {
            val observed = b.next<RoomView> { hitObserved(it) }
            check(result == observed) { "Hit views differ" }
            return result
        }
        val rejection = result as ActionRejected
        sameState(before, b.view())
        if (rejection.reason != Rules.FIRE_TOO_SOON) error("Shot rejected: ${rejection.reason}")
        check(rejection.retryAfterMs > 0) { "Fire cooldown rejection omitted RetryAfterMs" }
        delay(rejection.retryAfterMs.toLong())
    }
}

private suspend fun rejectShot(
    a: Client,
    b: Client,
    sequence: Long,
    reason: String,
    target: String = b.playerId,
) {
    val before = b.view()
    a.clear()
    a.send(ShootRequest(target, sequence))
    val rejection = a.next<ActionRejected>()
    check(rejection.reason == reason) { "Wrong shot rejection: ${rejection.reason}" }
    check(rejection.retryAfterMs == 0) { "Non-cooldown shot rejection returned RetryAfterMs" }
    sameState(before, b.view())
}

private fun both(a: Client, b: Client, predicate: (RoomView) -> Boolean): RoomView {
    val first = a.next(predicate)
    check(first == b.next(predicate)) { "Room views differ" }
    return first
}

private fun sameState(before: RoomView, after: RoomView) {
    check(
        before.roomId == after.roomId &&
            before.players == after.players &&
            before.phase == after.phase &&
            before.winner == after.winner
    ) { "Room state changed" }
}

private fun distance(a: PlayerView, b: PlayerView): Double = hypot(a.x - b.x, a.y - b.y)

private fun connected(a: Client, b: Client) {
    check(
        a.connector.isConnected &&
            b.connector.isConnected &&
            a.disconnects.get() == 0 &&
            b.disconnects.get() == 0
    ) { "Client disconnected" }
}

private fun player(view: RoomView, client: Client) =
    view.players.single { it.playerId == client.playerId }

private suspend fun rejectAction(player: Client, request: Any, reason: String): ActionRejected {
    val before = player.view()
    player.clear()
    player.send(request)
    val rejected = player.next<ActionRejected>()
    check(rejected.reason == reason) { "Expected $reason; received ${rejected.reason}." }
    val timeLimited = reason == Rules.MOVE_BUDGET || reason == Rules.FIRE_TOO_SOON
    check(if (timeLimited) rejected.retryAfterMs > 0 else rejected.retryAfterMs == 0) {
        "Unexpected RetryAfterMs for $reason."
    }
    sameState(before, player.view())
    return rejected
}


private suspend fun playWall(a: Client, b: Client) {
    moveTo(a, b, a, Rules.WALL_X - Rules.MAX_STEP, WALL_CENTER_Y)
    moveTo(a, b, b, Rules.WALL_X + Rules.MAX_STEP, WALL_CENTER_Y)
    val before = a.view()
    val target = player(before, b)
    rejectShot(a, b, player(before, a).lastSequence + 1, Rules.BLOCKED)
    rejectShot(b, a, player(before, b).lastSequence + 1, Rules.BLOCKED)
    val after = a.view()
    sameState(before, after)
    check(player(after, b).hp == target.hp) { "Wall-blocked shots changed HP" }
    moveTo(a, b, a, Rules.SPAWN_AX, Rules.SPAWN_AY)
    moveTo(a, b, b, Rules.SPAWN_BX, Rules.SPAWN_BY)
    val ready = a.view()
    val expectedHp = player(ready, b).hp - Rules.DAMAGE
    val shot = hit(a, b, player(ready, a).lastSequence + 1, expectedHp)
    check(player(shot, b).hp == expectedHp) { "Wall behind target blocked a clear shot" }
}
