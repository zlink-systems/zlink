package MyGame.lobby.application

import MyGame.*
import MyGame.match.domain.HttpRoomSnapshot
import MyGame.match.domain.RoomRoster
import MyGame.match.domain.RoomState
import MyGame.match.domain.RosterSnapshot
import java.util.concurrent.CompletableFuture
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class LobbyDomainTest {
    @Test
    fun lobbyDomainBoundaries() {
        var count = 0
        fun checkBoundary(condition: Boolean, name: String) {
            assertTrue(condition, "HTTP domain boundary failed: $name")
            count++
        }
        fun rejects(action: () -> Unit): Boolean =
            try {
                action()
                false
            } catch (_: IllegalArgumentException) {
                true
            } catch (_: IllegalStateException) {
                true
            }
        fun equal(left: Any, right: Any) =
            Json.mapper.readTree(Json.mapper.writeValueAsBytes(left)) ==
                Json.mapper.readTree(Json.mapper.writeValueAsBytes(right))

        val roster = RoomRoster()
        checkBoundary(!roster.canAdmit("a") && !roster.canAdmit("outsider"), "unprepared Room rejects admission")
        checkBoundary(rejects { roster.prepare("room", PrepareRoom("other", listOf("a", "b"))) }, "wrong match identity")
        checkBoundary(rejects { roster.prepare("room", PrepareRoom("room", listOf("a", "a"))) }, "duplicate roster member")
        checkBoundary(rejects { roster.prepare("room", PrepareRoom("room", listOf("a"))) }, "incomplete roster")
        checkBoundary(roster.prepare("room", PrepareRoom("room", listOf("a", "b"))).matchId == "room", "preparation acknowledges identity")
        val members = mutableListOf("a", "b")
        val ownedRoster = RoomRoster()
        ownedRoster.prepare("owned", PrepareRoom("owned", members))
        members[0] = "outsider"
        checkBoundary(ownedRoster.canAdmit("a") && ownedRoster.canAdmit("b") && !ownedRoster.canAdmit("outsider"), "roster owns member copy")
        val snapshot = roster.capture()
        roster.prepare("room", PrepareRoom("room", listOf("b", "a")))
        checkBoundary(equal(snapshot, roster.capture()), "same preparation is idempotent")
        checkBoundary(
            rejects { roster.prepare("room", PrepareRoom("room", listOf("a", "outsider"))) } && equal(snapshot, roster.capture()),
            "prepared roster cannot be replaced",
        )

        var now = 1000.0
        val game = RoomState { now }
        game.admit("a")
        game.admit("b")
        game.move("a", MoveRequest(Vec3(0.0, 0.0, -1.0), 1))
        game.shoot("a", ShootRequest(Vec3(4.0, 0.0, 1.0), 2))
        val envelope = HttpRoomSnapshot(game.capture(), roster.capture())
        val restored = Json.mapper.readValue(
            Json.mapper.writeValueAsBytes(envelope),
            HttpRoomSnapshot::class.java,
        )
        val restoredRoster = RoomRoster()
        restoredRoster.restore(restored.roster)
        now = 900000.0
        val restoredGame = RoomState { now }
        restoredGame.restore(restored.game)
        checkBoundary(
            restoredRoster.canAdmit("a") && restoredRoster.canAdmit("b") &&
                !restoredRoster.canAdmit("outsider") && equal(game.view(), restoredGame.view()),
            "relocation envelope retains roster, HP, position and sequence",
        )
        checkBoundary(rejects { restoredRoster.prepare("room", PrepareRoom("room", listOf("a", "outsider"))) }, "relocated roster cannot be replaced")
        checkBoundary(rejects { RoomRoster().restore(RosterSnapshot(null, listOf("a"))) }, "unprepared snapshot with members")
        checkBoundary(
            Json.mapper.readValue("""{"playerId":"default"}""", MatchRequest::class.java).mode == Names.DEFAULT_MODE,
            "omitted HTTP mode defaults to duel",
        )

        val provisioner = RecordingProvisioner()
        val firstProcess = Matchmaker(provisioner)
        val rankedA = firstProcess.match(MatchRequest("ra", "ranked")).toCompletableFuture()
        val casualA = firstProcess.match(MatchRequest("ca", "casual")).toCompletableFuture()
        val rankedB = firstProcess.match(MatchRequest("rb", "ranked")).toCompletableFuture()
        checkBoundary(!casualA.isDone && rankedA.join() == rankedB.join(), "interleaved modes pair only matching members")
        val casualB = firstProcess.match(MatchRequest("cb", "casual")).toCompletableFuture()
        checkBoundary(casualA.join() == casualB.join() && casualA.join().roomId != rankedA.join().roomId, "different modes have distinct Rooms")
        checkBoundary(
            provisioner.calls[0].members.toSet() == setOf("ra", "rb") &&
                provisioner.calls[1].members.toSet() == setOf("ca", "cb"),
            "provisioner receives each matched roster",
        )
        val restarted = Matchmaker(provisioner)
        val newA = restarted.match(MatchRequest("new-a")).toCompletableFuture()
        val newB = restarted.match(MatchRequest("new-b")).toCompletableFuture()
        val newRoom = newA.join()
        checkBoundary(
            newRoom == newB.join() && newRoom.roomId != rankedA.join().roomId && newRoom.roomId != casualA.join().roomId,
            "restart cannot reuse counter-based Room identity",
        )
        checkBoundary(
            provisioner.calls.all { it.roomId.startsWith(Names.ROOM_PREFIX) && it.roomId.removePrefix(Names.ROOM_PREFIX).matches(Regex("[0-9a-f]{32}")) },
            "Room ID contains a generated GUID",
        )

        val pendingPreparation = CompletableFuture<MatchResponse>()
        val heldProvisioner = RecordingProvisioner(pendingPreparation)
        val held = Matchmaker(heldProvisioner)
        val heldA = held.match(MatchRequest("held-a")).toCompletableFuture()
        val heldB = held.match(MatchRequest("held-b")).toCompletableFuture()
        checkBoundary(!heldA.isDone && !heldB.isDone && heldProvisioner.calls.size == 1, "HTTP assignments wait for Room preparation completion")
        val pendingRoomId = heldProvisioner.calls.single().roomId
        pendingPreparation.complete(MatchResponse(pendingRoomId, RecordingProvisioner.SESSION_ENDPOINT))
        checkBoundary(heldA.join() == heldB.join(), "prepared assignment releases both HTTP responses")

        assertEquals(19, count, "Lobby domain boundary count")
        println("HTTP domain boundaries: $count/$count passed")
    }

    private class RecordingProvisioner(
        private val completion: CompletableFuture<MatchResponse>? = null,
    ) : RoomProvisioner {
        data class Call(val roomId: String, val members: List<String>)

        val calls = mutableListOf<Call>()

        override fun prepare(roomId: String, members: List<String>) =
            completion?.also { calls += Call(roomId, members.toList()) }
                ?: CompletableFuture.completedFuture(
                    MatchResponse(roomId, SESSION_ENDPOINT).also { calls += Call(roomId, members.toList()) },
                )

        companion object {
            const val SESSION_ENDPOINT = "ws://127.0.0.1:1"
        }
    }
}
