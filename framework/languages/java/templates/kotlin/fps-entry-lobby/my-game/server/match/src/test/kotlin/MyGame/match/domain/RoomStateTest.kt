package MyGame.match.domain

import MyGame.*
import kotlin.math.abs
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class RoomStateTest {
    @Test
    fun fpsDomainBoundariesMatchTheApprovedRules() {
        var checkedCount = 0
        fun checkBoundary(condition: Boolean, name: String) {
            if (!condition) error("Domain boundary failed: $name")
            checkedCount++
        }
        fun equal(a: Any?, b: Any?) = Json.mapper.writeValueAsString(a) == Json.mapper.writeValueAsString(b)

        val box = Aabb(Vec3(1.0, 1.0, 1.0), Vec3(2.0, 2.0, 2.0))
        checkBoundary(box.intersect(Vec3(0.0, 1.0, 1.0), Vec3(1.0, 0.0, 0.0), 1.0) == 1.0,
            "closed face and exact limit")
        checkBoundary(box.intersect(Vec3(0.0, 2.0, 2.0), Vec3(1.0, 0.0, 0.0), 3.0) == 1.0,
            "parallel corner tangent")
        checkBoundary(box.intersect(Vec3(0.0, 2.001, 2.0), Vec3(1.0, 0.0, 0.0), 3.0) == null,
            "parallel outside slab")
        checkBoundary(box.intersect(Vec3(1.5, 1.5, 1.5), Vec3(1.0, 0.0, 0.0), 3.0) == 0.0,
            "inside origin")
        checkBoundary(box.intersect(Vec3(3.0, 1.0, 1.0), Vec3(-1.0, 0.0, 0.0), 1.0) == 1.0,
            "negative direction")
        checkBoundary(box.intersect(Vec3(0.0, 1.0, 1.0), Vec3(-1.0, 0.0, 0.0), 3.0) == null,
            "box behind ray")
        checkBoundary(box.intersect(Vec3(0.0, 1.0, 1.0), Vec3(1.0, 0.0, 0.0), 0.999) == null,
            "range cutoff")
        checkBoundary(Vec3(Double.MIN_VALUE, 0.0, 0.0).length == Double.MIN_VALUE,
            "subnormal movement length")
        val huge = Vec3(Double.MAX_VALUE, Double.MAX_VALUE, 0.0).normalizedOrNull()
        checkBoundary(huge != null && abs(huge.length - 1.0) < 1e-12,
            "large finite aim normalization")
        val tiny = Vec3(Double.MIN_VALUE, 0.0, 0.0).normalizedOrNull()
        checkBoundary(tiny == Vec3(1.0, 0.0, 0.0), "subnormal aim normalization")
        checkBoundary(
            Vec3(Double.NaN, 0.0, 0.0).normalizedOrNull() == null &&
                Vec3(Double.POSITIVE_INFINITY, 0.0, 0.0).normalizedOrNull() == null &&
                Vec3().normalizedOrNull() == null,
            "invalid aim",
        )
        ArenaMap.spawns.forEach { spawn ->
            checkBoundary(ArenaMap.bounds.contains(spawn) && ArenaMap.walls.none { it.contains(spawn) },
                "spawn outside walls and inside map")
        }

        var now = 1000.0
        fun positioned(a: Vec3, b: Vec3): RoomState =
            RoomState { now }.also {
                it.restore(
                    RoomSnapshot(
                        listOf(
                            CombatantSnapshot("a", a, 100, 0, 0.0, 0.0),
                            CombatantSnapshot("b", b, 100, 0, 0.0, 0.0),
                        ),
                        MatchPhase.Playing,
                        null,
                    ),
                )
            }
        val room = RoomState { now }
        room.admit("a")
        room.admit("b")
        checkBoundary(room.move("a", MoveRequest(Vec3(0.0, 0.0, -2.0), 1)) == null,
            "first movement full budget")
        var before = room.capture()
        val budget = room.move("a", MoveRequest(Vec3(0.0, 0.0, -2.0), 2))
        checkBoundary(budget?.reason == Rejection.MOVE_BUDGET && budget.retryAfterMs == 200 &&
            equal(before, room.capture()), "movement rejection leaves timing and sequence unchanged")
        val tinyMove = room.move("a", MoveRequest(Vec3(Double.MIN_VALUE, 0.0, 0.0), 2))
        checkBoundary(tinyMove?.reason == Rejection.MOVE_BUDGET && tinyMove.retryAfterMs > 0 &&
            equal(before, room.capture()), "nonzero subnormal movement with zero elapsed budget leaves state unchanged")
        now += 100.0
        checkBoundary(room.move("a", MoveRequest(Vec3(0.0, 0.0, -1.0), 2)) == null,
            "partial elapsed movement budget")
        before = room.capture()
        checkBoundary(room.move("a", MoveRequest(Vec3(0.0, 0.0, -3.0), 3))?.reason == Rejection.STEP_TOO_LARGE &&
            equal(before, room.capture()), "oversize rejection invariant")
        checkBoundary(room.move("a", MoveRequest(Vec3(0.0, 1.0, 0.0), 3))?.reason == Rejection.VERTICAL_MOVE &&
            equal(before, room.capture()), "upward movement rejection invariant")
        checkBoundary(room.move("a", MoveRequest(Vec3(0.0, -1.0, 0.0), 3))?.reason == Rejection.VERTICAL_MOVE &&
            equal(before, room.capture()), "downward movement rejection invariant")
        val edge = positioned(Vec3(0.0, 0.0, 65.0), Vec3(80.0, 0.0, 65.0))
        val edgeBefore = edge.capture()
        checkBoundary(edge.move("a", MoveRequest(Vec3(-1.0, 0.0, 0.0), 1))?.reason == Rejection.OUTSIDE_MAP &&
            equal(edgeBefore, edge.capture()), "horizontal map boundary invariant")
        checkBoundary(room.move("a", MoveRequest(Vec3(Double.NaN, 0.0, 0.0), 3))?.reason == Rejection.NON_FINITE &&
            equal(before, room.capture()), "nonfinite movement invariant")
        checkBoundary(room.shoot("a", ShootRequest(Vec3(0.0, 1.0, 0.0), 3)).result?.outcome == ShotOutcome.MISS,
            "miss is accepted")
        before = room.capture()
        checkBoundary(room.shoot("a", ShootRequest(Vec3(1.0, 0.0, 0.0), 3)).rejected?.reason == Rejection.STALE_SEQUENCE &&
            equal(before, room.capture()), "shared stale sequence invariant")
        checkBoundary(room.shoot("a", ShootRequest(Vec3(), 4)).rejected?.reason == Rejection.INVALID_AIM &&
            equal(before, room.capture()), "invalid aim leaves cooldown unchanged")
        val tooSoon = room.shoot("a", ShootRequest(Vec3(0.0, 1.0, 0.0), 4)).rejected
        checkBoundary(tooSoon?.reason == Rejection.FIRE_TOO_SOON &&
            tooSoon.retryAfterMs == Game.FIRE_INTERVAL_MS && equal(before, room.capture()),
            "fire interval invariant")
        now += Game.FIRE_INTERVAL_MS
        checkBoundary(room.shoot("a", ShootRequest(Vec3(0.0, 1.0, 0.0), 4)).result != null,
            "exact fire interval")
        checkBoundary(room.move("a", MoveRequest(Vec3(0.0, 0.0, -2.0), 5)) == null,
            "movement budget capped at max step")

        val tie = positioned(Vec3(48.0, 0.0, 22.0), Vec3(49.5, 0.0, 22.0))
        checkBoundary(tie.shoot("a", ShootRequest(Vec3(1.0, 0.0, 0.0), 1)).result?.outcome ==
            ShotOutcome.BLOCKED_BY_WALL && tie.view()[1].hp == 100,
            "equal wall and player distance selects wall")
        val behind = positioned(ArenaMap.spawns[0], ArenaMap.spawns[1])
        checkBoundary(behind.shoot("a", ShootRequest(Vec3(1.0, 0.0, 0.0), 1)).result?.outcome == ShotOutcome.HIT &&
            behind.view()[1].hp == 75, "wall behind target does not block")
        val exactRange = positioned(Vec3(0.0, 0.0, 65.0), Vec3(80.5, 0.0, 65.0))
        checkBoundary(exactRange.shoot("a", ShootRequest(Vec3(1.0, 0.0, 0.0), 1)).result?.outcome == ShotOutcome.HIT,
            "player face at exact range")
        val beyondRange = positioned(Vec3(0.0, 0.0, 65.0), Vec3(80.501, 0.0, 65.0))
        checkBoundary(beyondRange.shoot("a", ShootRequest(Vec3(1.0, 0.0, 0.0), 1)).result?.outcome == ShotOutcome.MISS,
            "player face beyond range")
        val crossing = positioned(Vec3(48.0, 0.0, 22.0), ArenaMap.spawns[1])
        before = crossing.capture()
        checkBoundary(crossing.move("a", MoveRequest(Vec3(2.0, 0.0, 0.0), 1))?.reason ==
            Rejection.MOVE_BLOCKED_BY_WALL && equal(before, crossing.capture()),
            "movement endpoint inside wall")
        val touching = positioned(Vec3(48.0, 0.0, 20.0), ArenaMap.spawns[1])
        checkBoundary(touching.move("a", MoveRequest(Vec3(1.0, 0.0, 0.0), 1))?.reason ==
            Rejection.MOVE_BLOCKED_BY_WALL, "movement touches wall face")

        now = 1000.0
        val source = RoomState { now }
        source.admit("a")
        source.admit("b")
        source.move("a", MoveRequest(Vec3(0.0, 0.0, -2.0), 1))
        source.shoot("a", ShootRequest(Vec3(0.0, 1.0, 0.0), 2))
        now += 60.0
        val snapshot = source.capture()
        checkBoundary(snapshot.players[0].fireRemainingMs == 190.0 &&
            snapshot.players[0].moveRemainingMs == 140.0, "snapshot stores remaining milliseconds")
        checkBoundary(equal(snapshot, source.capture()), "capture does not mutate live state")
        var destinationNow = 900000.0
        val destination = RoomState { destinationNow }
        destination.restore(Json.mapper.readValue(Json.mapper.writeValueAsBytes(snapshot), RoomSnapshot::class.java))
        checkBoundary(equal(source.view(), destination.view()) && source.phase == destination.phase,
            "relocation keeps view under new clock origin")
        val relocatedMove = destination.move("a", MoveRequest(Vec3(0.0, 0.0, -2.0), 3))
        checkBoundary(relocatedMove?.reason == Rejection.MOVE_BUDGET && relocatedMove.retryAfterMs == 140,
            "relocated movement budget")
        val relocatedShot = destination.shoot("a", ShootRequest(Vec3(0.0, 1.0, 0.0), 3)).rejected
        checkBoundary(relocatedShot?.reason == Rejection.FIRE_TOO_SOON && relocatedShot.retryAfterMs == 190,
            "relocated fire cooldown")
        destinationNow += 140.0
        checkBoundary(destination.move("a", MoveRequest(Vec3(0.0, 0.0, -2.0), 3)) == null,
            "relocated budget exact boundary")
        destinationNow += 50.0
        checkBoundary(destination.shoot("a", ShootRequest(Vec3(0.0, 1.0, 0.0), 4)).result != null,
            "relocated cooldown exact boundary")
        val lethal = positioned(ArenaMap.spawns[0], ArenaMap.spawns[1])
        for (sequence in 1L..4L) {
            lethal.shoot("a", ShootRequest(Vec3(1.0, 0.0, 0.0), sequence))
            now += Game.FIRE_INTERVAL_MS
        }
        checkBoundary(lethal.phase == MatchPhase.Finished && lethal.winner == "a" && lethal.view()[1].hp == 0,
            "four hits finish match")
        before = lethal.capture()
        checkBoundary(lethal.move("a", MoveRequest(Vec3(0.0, 0.0, 1.0), 5))?.reason == Rejection.MATCH_FINISHED &&
            lethal.shoot("a", ShootRequest(Vec3(1.0, 0.0, 0.0), 5)).rejected?.reason == Rejection.MATCH_FINISHED &&
            equal(before, lethal.capture()), "finished actions leave state unchanged")
        val finished = RoomState { destinationNow }
        finished.restore(before)
        checkBoundary(finished.phase == MatchPhase.Finished && finished.winner == "a" &&
            equal(finished.view(), lethal.view()), "finished snapshot preserves winner and sequence")
        checkBoundary(room.view().plus(destination.view()).plus(finished.view()).all { it.position.y == 0.0 },
            "feet remain on ground through movement and relocation")
        val bounded = RoomState { 0.0 }
        bounded.admit("a")
        bounded.admit("b")
        assertEquals(null, bounded.move("a", MoveRequest(Vec3(0.0, 0.0, -1.0), Game.MAX_SEQUENCE)))
        val boundedBefore = bounded.capture()
        for (invalid in listOf(Game.MAX_SEQUENCE + 1)) {
            assertEquals(Rejection.INTEGER_OUT_OF_RANGE, bounded.move("a", MoveRequest(Vec3(1.0, 0.0, 0.0), invalid))?.reason)
            assertEquals(Rejection.INTEGER_OUT_OF_RANGE, bounded.shoot("a", ShootRequest(Vec3(1.0, 0.0, 0.0), invalid)).rejected?.reason)
            assertTrue(equal(boundedBefore, bounded.capture()))
        }
        val boundedShot = RoomState { 0.0 }
        boundedShot.admit("a")
        boundedShot.admit("b")
        checkBoundary(bounded.view().first().lastSequence == Game.MAX_SEQUENCE &&
            boundedShot.shoot("a", ShootRequest(Vec3(0.0, 1.0, 0.0), Game.MAX_SEQUENCE)).result?.sequence == Game.MAX_SEQUENCE,
            "safe sequence upper bound and invalid inputs preserve state")
        assertEquals(46, checkedCount)
        println("Domain boundaries: $checkedCount/46 passed")
    }

}
