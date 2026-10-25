package MyGame.e2e

import MyGame.*
import MyGame.lobby.application.Matchmaker
import MyGame.lobby.application.RoomProvisioner
import MyGame.match.domain.RoomRoster
import MyGame.match.domain.RoomState
import MyGame.match.infrastructure.zlink.RoomTransferSnapshot
import java.util.concurrent.CompletableFuture

object LobbyDomainChecks {
    fun run() {
        var count = 0
        fun expect(valid: Boolean, label: String) {
            check(valid) { "HTTP domain check failed: $label" }
            count++
        }
        fun rejects(action: () -> Unit): Boolean = try {
            action()
            false
        } catch (expected: IllegalArgumentException) {
            true
        } catch (expected: IllegalStateException) {
            true
        }

        val roster = RoomRoster()
        expect(!roster.canAdmit("a") && !roster.canAdmit("outsider"), "unprepared Room rejects admission")
        expect(rejects { roster.prepare("room", PrepareRoom("other", listOf("a", "b"))) }, "wrong match identity")
        expect(rejects { roster.prepare("room", PrepareRoom("room", listOf("a", "a"))) }, "duplicate roster member")
        expect(rejects { roster.prepare("room", PrepareRoom("room", listOf("a"))) }, "incomplete roster")
        val members = mutableListOf("a", "b")
        expect(roster.prepare("room", PrepareRoom("room", members)) == RoomPrepared("room"), "preparation acknowledges identity")
        members[0] = "outsider"
        expect(roster.canAdmit("a") && roster.canAdmit("b") && !roster.canAdmit("outsider"), "RoomRoster owns a copy of its member list")
        val before = roster.capture()
        roster.prepare("room", PrepareRoom("room", listOf("b", "a")))
        expect(before == roster.capture(), "same preparation is idempotent")
        expect(rejects { roster.prepare("room", PrepareRoom("room", listOf("a", "outsider"))) } && before == roster.capture(), "prepared roster cannot be replaced")

        val game = RoomState { 0L }
        game.admit("a")
        game.admit("b")
        game.move("a", MoveRequest(1.0, 0.0, 1))
        game.shoot("a", ShootRequest("b", 2))
        val envelope = RoomTransferSnapshot(Json.mapper.readTree(game.capture()), roster.capture())
        val encoded = Json.mapper.writeValueAsBytes(envelope)
        val fields = Json.mapper.readTree(encoded)
        val restored = Json.mapper.readValue(encoded, RoomTransferSnapshot::class.java)
        val restoredRoster = RoomRoster().also { it.restore(restored.roster) }
        val restoredGame = RoomState { 900_000_000_000L }.also {
            it.restore(Json.mapper.writeValueAsBytes(restored.game))
        }
        expect(
            fields.has("game") && fields.has("roster") &&
                restoredRoster.canAdmit("a") && restoredRoster.canAdmit("b") &&
                !restoredRoster.canAdmit("outsider") && game.view() == restoredGame.view() &&
                game.phase == restoredGame.phase && game.winner == restoredGame.winner &&
                game.view().first().lastSequence == 2L,
            "relocation envelope retains roster, state and sequence",
        )
        expect(rejects { restoredRoster.prepare("room", PrepareRoom("room", listOf("a", "outsider"))) }, "relocated roster cannot be replaced")
        expect(rejects { RoomRoster().restore(RoomRoster.RosterSnapshot(null, listOf("a"))) }, "unprepared snapshot with members")

        data class Call(val roomId: String, val members: List<String>)
        val calls = mutableListOf<Call>()
        val provisioner = RoomProvisioner { id, matched ->
            calls += Call(id, matched.toList())
            CompletableFuture.completedFuture(MatchResponse(id, "ws://127.0.0.1:1"))
        }
        Matchmaker(provisioner).use { matches ->
            val defaultA = matches.match(MatchRequest("default-a")).toCompletableFuture()
            val defaultB = matches.match(MatchRequest("default-b", Names.DEFAULT_MODE)).toCompletableFuture()
            expect(defaultA.join() == defaultB.join(), "omitted HTTP mode defaults to duel")
            val rankedA = matches.match(MatchRequest("ra", "ranked")).toCompletableFuture()
            val casualA = matches.match(MatchRequest("ca", "casual")).toCompletableFuture()
            val rankedB = matches.match(MatchRequest("rb", "ranked")).toCompletableFuture()
            expect(!casualA.isDone && rankedA.join() == rankedB.join(), "interleaved modes pair only matching members")
            val casualB = matches.match(MatchRequest("cb", "casual")).toCompletableFuture()
            expect(casualA.join() == casualB.join() && casualA.join().roomId != rankedA.join().roomId, "different modes receive distinct Rooms")
            expect(
                listOf(setOf("default-a", "default-b"), setOf("ra", "rb"), setOf("ca", "cb")).all { expected ->
                    calls.any { it.members.toSet() == expected }
                },
                "provisioner receives each matched roster",
            )
            Matchmaker(provisioner).use { restarted ->
                val newA = restarted.match(MatchRequest("new-a")).toCompletableFuture()
                val newB = restarted.match(MatchRequest("new-b")).toCompletableFuture()
                expect(newA.join() == newB.join() && newA.join().roomId != rankedA.join().roomId && newA.join().roomId != casualA.join().roomId, "restarted lobby creates a new Room ID")
            }
        }
        expect(calls.all { Regex("room-[0-9a-f]{32}").matches(it.roomId) }, "Room ID uses prefix and lowercase UUID")
        val pending = CompletableFuture<MatchResponse>()
        val heldCalls = mutableListOf<Call>()
        Matchmaker { id, matched ->
            heldCalls += Call(id, matched.toList())
            pending
        }.use { matches ->
            val a = matches.match(MatchRequest("held-a")).toCompletableFuture()
            val b = matches.match(MatchRequest("held-b")).toCompletableFuture()
            expect(!a.isDone && !b.isDone && heldCalls.size == 1, "HTTP assignments wait for RoomPrepared")
            pending.complete(MatchResponse(heldCalls.single().roomId, "ws://127.0.0.1:1"))
            expect(a.join() == b.join(), "preparation releases both HTTP responses")
        }
        check(count == 19) { "Expected 19 HTTP checks, received $count" }
        println("HTTP domain boundaries: $count/$count passed")
    }
}
