package MyGame.e2e

import MyGame.*

private const val PLAYER_ONE = "p1"
private const val PLAYER_TWO = "p2"
private const val PLAYER_THREE = "p3"
private const val PLAYER_FOUR = "p4"
private const val FOREST = "forest"
private val MOVED_X = Names.MAX_STEP.toDouble()

private fun sameState(before: RegionView, after: RegionView) {
    check(before.regionId == after.regionId && before.players == after.players) {
        "Region state changed"
    }
}

private fun sameActor(before: SessionInfo, after: SessionInfo) {
    check(before.actorId == after.actorId && before.objectGeneration == after.objectGeneration) {
        "Actor identity changed"
    }
}

suspend fun scenarios(selected: List<String>, nodes: Nodes): Int {
    val clients = mutableListOf<Client>()
    val completed = mutableSetOf<String>()
    var primary: Throwable? = null
    suspend fun create(id: String): Client {
        val client = Client(nodes.endpoint, id)
        clients += client
        client.connect()
        return client
    }
    try {
        var p1 = create(PLAYER_ONE)
        val first = p1.join(FOREST)
        check(first.regionId == Names.regionId(FOREST, Names.FIRST_REGION_INDEX)) {
            "First Region mismatch"
        }
        val p2 = create(PLAYER_TWO)
        val together = p2.join(FOREST)
        check(
            together.regionId == first.regionId &&
                together.players ==
                    listOf(PlayerPosition(PLAYER_ONE, 0.0, 0), PlayerPosition(PLAYER_TWO, 0.0, 0))
        ) {
            "Players are not together"
        }
        sameState(together, p1.next<RegionView> { it.players.size == 2 })
        val p3 = create(PLAYER_THREE)
        val isolated = p3.join(FOREST)
        check(
            isolated.regionId == Names.regionId(FOREST, 2) &&
                isolated.players == listOf(PlayerPosition(PLAYER_THREE, 0.0, 0))
        ) {
            "Capacity did not isolate p3"
        }
        var session = p1.sessionInfo()
        val session2 = p2.sessionInfo()
        p1.send(MoveRequest(MOVED_X, 1))
        p1.send(MoveRequest(MOVED_X, 2))
        val firstMove = p1.next<RegionView> { v -> v.players.any { it.playerId == PLAYER_ONE && it.sequence == 1L } }
        val rejected = p1.next<ActionRejected> { it.reason == "move-budget" }
        check(rejected.retryAfterMs > 0) { "Missing movement retry time" }
        sameState(firstMove, p1.view())
        kotlinx.coroutines.delay(rejected.retryAfterMs.toLong())
        val moved = p1.move(MOVED_X, 2)
        check(moved.players.any { it.playerId == PLAYER_ONE && it.x == MOVED_X * 2 && it.sequence == 2L }) { "Retry did not preserve sequence" }
        sameState(
            moved,
            p2.next<RegionView> { v ->
                v.players.any { it.playerId == PLAYER_ONE && it.x == MOVED_X * 2 }
            },
        )
        sameState(isolated, p3.view())
        check(
            p3.history().filterIsInstance<RegionView>().all { view ->
                view.players.all { it.playerId == PLAYER_THREE }
            }
        ) {
            "p3 received another Region's movement"
        }
        for (scenario in selected) {
            when (scenario) {
                Names.REGION_SCENARIO -> {
                    val p4 = create("capacity-p4")
                    val full = p4.join(FOREST)
                    check(full.regionId == Names.regionId(FOREST, 2) && full.players.size == 2)
                    val p5 = create("capacity-p5")
                    p5.send(EnterRegionRequest(p5.playerId, FOREST))
                    check(p5.next<RegionUnavailable> { true } == RegionUnavailable(FOREST, "capacity-exhausted"))
                    check(p5.sessionInfo().actorId == p5.playerId)
                    val unknown = create("unknown-p6")
                    unknown.send(EnterRegionRequest(unknown.playerId, "unknown"))
                    check(unknown.next<RegionUnavailable> { true } == RegionUnavailable("unknown", "region-unknown"))
                    unknown.send(RegionFull(Names.REGION_FULL, FOREST, 1))
                    check(unknown.next<ActionRejected> { true } == ActionRejected("packet-not-allowed", 0))
                    check(unknown.sessionInfo().actorId == unknown.playerId)
                }
                Names.MOVEMENT -> Unit
                Names.DUNGEON_SCENARIO -> {
                    val id = "dungeon-1"
                    val opened = p1.dungeon(id)
                    check(opened.dungeonId == id && opened.nodeId == Roles.INSTANCE) {
                        "Dungeon host mismatch"
                    }
                }
                Names.RECONNECT -> {
                    p1.close()
                    clients.remove(p1)
                    p1 = create(PLAYER_ONE)
                    val restored = p1.join(FOREST)
                    sameState(moved, restored)
                    check(restored.nodeId == moved.nodeId) { "Reconnect changed Region host" }
                    val identity = p1.sessionInfo()
                    sameActor(session, identity)
                    check(identity.sessionId != session.sessionId) {
                        "Reconnect reused physical Session"
                    }
                    session = identity
                }
                Names.UPDATE -> {
                    nodes.drain(moved.nodeId)
                    val relocated = p1.view { it.nodeId != moved.nodeId }
                    sameState(moved, relocated)
                    sameState(relocated, p2.view { it.nodeId == relocated.nodeId })
                    check(relocated.nodeId != moved.nodeId) { "Region did not relocate" }
                    check(session == p1.sessionInfo() && session2 == p2.sessionInfo()) {
                        "Relocation changed Sessions"
                    }
                    check(p1.disconnects == 0 && p2.disconnects == 0) {
                        "Drain disconnected clients"
                    }
                    nodes.replace(moved.nodeId)
                    sameState(relocated, p1.view())
                    val fresh = create(PLAYER_FOUR)
                    val admitted = fresh.join("desert")
                    check(
                        admitted.nodeId == moved.nodeId &&
                            admitted.players == listOf(PlayerPosition(PLAYER_FOUR, 0.0, 0))
                    ) {
                        "New Region was not accepted by the replacement Region node"
                    }
                }
            }
            check(clients.all { it.disconnects == 0 }) { "Client disconnected" }
            completed += scenario
            println("✓ $scenario")
        }
    } catch (failure: Exception) {
        primary = failure
        failure.printStackTrace(System.err)
    } catch (failure: Error) {
        primary = failure
        throw failure
    } finally {
        for (scenario in selected) if (scenario !in completed) println("✗ $scenario")
        var cleanup: Exception? = null
        for (client in clients.asReversed()) {
            try {
                client.close()
            } catch (failure: Exception) {
                if (cleanup == null) cleanup = failure else cleanup.addSuppressed(failure)
            }
        }
        cleanup?.let {
            if (primary != null) {
                primary.addSuppressed(it)
                it.printStackTrace(System.err)
            } else throw it
        }
    }
    return completed.size
}
