package MyGame.e2e

import MyGame.*
import MyGame.field.FieldPositionDomain
import MyGame.field.FieldPositionSnapshot
import MyGame.field.PartyRun
import MyGame.instance.DungeonRun
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong

internal object DomainChecks {
    fun run(): Int {
        var count = 0
        fun verify(condition: Boolean, message: String) {
            check(condition) { "Domain: $message" }
            count++
        }

        val now = AtomicLong(1000)
        val domain = FieldPositionDomain("field-1", now::get)
        domain.addPlayer("p", EnterField(50.0, 0L))
        val invalidMoves =
            listOf(
                MoveRequest(Double.NaN, 1L),
                MoveRequest(Double.POSITIVE_INFINITY, 1L),
                MoveRequest(Double.NEGATIVE_INFINITY, 1L),
                MoveRequest(10.01, 1L),
                MoveRequest(0.0, Rules.MAX_SAFE_INTEGER + 1),
                MoveRequest(0.0, -1L),
                MoveRequest(0.0, 0L),
            )
        val invalidReasons =
            listOf(
                Rejection.NON_FINITE,
                Rejection.NON_FINITE,
                Rejection.NON_FINITE,
                Rejection.STEP_TOO_LARGE,
                Rejection.INTEGER_OUT_OF_RANGE,
                Rejection.STALE_SEQUENCE,
                Rejection.STALE_SEQUENCE,
            )
        for ((request, reason) in invalidMoves.zip(invalidReasons)) {
            val before = domain.state("p")
            val rejected = domain.move("p", request)
            verify(
                rejected.rejection?.reason == reason && rejected.rejection?.retryAfterMs == 0,
                "movement rejection $reason",
            )
            verify(domain.state("p") == before, "movement rejection changed state")
        }
        verify(domain.move("p", MoveRequest(10.0, 1L)).accepted, "first move budget")
        val state = domain.state("p")
        val budget = domain.move("p", MoveRequest(10.0, 2L))
        verify(
            budget.rejection?.reason == Rejection.MOVE_BUDGET && budget.rejection?.retryAfterMs == 200,
            "budget retry",
        )
        verify(domain.state("p") == state, "budget rejection changed state")
        now.addAndGet(requireNotNull(budget.rejection).retryAfterMs.toLong())
        verify(domain.move("p", MoveRequest(10.0, 2L)).accepted, "retry after server delay")
        now.addAndGet(25)
        val snapshot =
            Json.mapper.readValue(
                Json.mapper.writeValueAsBytes(domain.capture()),
                FieldPositionSnapshot::class.java,
            )
        val restored = FieldPositionDomain("field-1") { now.get() + 10_000 }
        restored.restore(snapshot)
        verify(restored.state("p") == domain.state("p"), "relative time snapshot")
        verify(restored.move("p", MoveRequest(0.0, Rules.MAX_SAFE_INTEGER)).accepted, "safe integer upper bound")
        verify(
            restored.move("p", MoveRequest(0.0, Rules.MAX_SAFE_INTEGER + 1)).rejection?.reason ==
                Rejection.INTEGER_OUT_OF_RANGE,
            "range before stale",
        )
        verify(
            restored.move("p", MoveRequest(0.0, Rules.MAX_SAFE_INTEGER)).rejection?.reason ==
                Rejection.STALE_SEQUENCE,
            "maximum sequence stale",
        )
        domain.addPlayer("edge", EnterField(0.0, 0L))
        verify(domain.move("edge", MoveRequest(-1.0, 1L)).rejection?.reason == Rejection.OUTSIDE_WORLD, "world minimum")
        val last = FieldPositionDomain("field-4", now::get)
        last.addPlayer("edge", EnterField(399.0, 0L))
        verify(last.move("edge", MoveRequest(1.0, 1L)).rejection?.reason == Rejection.OUTSIDE_WORLD, "world maximum")
        last.addPlayer("fraction", EnterField(399.5, 0L))
        verify(last.move("fraction", MoveRequest(0.0, 1L)).accepted, "fractional world edge")
        verify(domain.queueArrival("x", EnterField(100.0, 0L))?.reason == Rejection.OUTSIDE_WORLD, "arrival field")
        verify(
            domain.queueArrival("x", EnterField(1.0, Rules.MAX_SAFE_INTEGER + 1))?.reason ==
                Rejection.INTEGER_OUT_OF_RANGE,
            "arrival sequence",
        )
        verify(
            domain.queueArrival("x", EnterField(1.0, 0L, 201))?.reason == Rejection.INTEGER_OUT_OF_RANGE,
            "arrival remaining time",
        )

        for (phase in RunPhase.values()) {
            val party =
                PartyRun(
                    members = mutableListOf("leader", "member"),
                    phase = phase,
                    runId = if (phase == RunPhase.Forming) null else "run-${"a".repeat(32)}",
                )
            val before = Json.mapper.writeValueAsString(party)
            val nonleader = party.enter("member")
            verify(
                nonleader?.reason == Rejection.NOT_LEADER && Json.mapper.writeValueAsString(party) == before,
                "nonleader immutable in $phase",
            )
            val oldRun = party.runId
            verify(party.enter("leader") == null, "leader entry $phase")
            verify(PartyRun.validId(party.runId, "run-"), "run ID")
            verify(
                if (phase == RunPhase.Forming || phase == RunPhase.Completed)
                    party.runId != oldRun && party.phase == RunPhase.Preparing
                else party.runId == oldRun && party.phase == phase,
                "entry transition $phase",
            )
            val leaving = PartyRun(members = mutableListOf("leader", "member"), phase = phase)
            leaving.leave("member")
            verify(leaving.members.size == if (phase == RunPhase.Forming) 1 else 2, "leave $phase")
            val transition = PartyRun(members = mutableListOf("leader"), phase = phase, runId = "run")
            verify(
                (transition.prepared("run") == null) == (phase == RunPhase.Preparing || phase == RunPhase.Entering),
                "prepared transition $phase",
            )
            transition.phase = phase
            transition.entered.add("leader")
            verify(
                (transition.started("run") == null) == (phase == RunPhase.Entering || phase == RunPhase.Active),
                "started transition $phase",
            )
            transition.phase = phase
            verify(
                (transition.completed("run") == null) == (phase == RunPhase.Active || phase == RunPhase.Completed),
                "completed transition $phase",
            )
            transition.phase = phase
            verify(
                transition.prepared("wrong")?.reason == Rejection.RUN_MISMATCH &&
                    transition.started("wrong")?.reason == Rejection.RUN_MISMATCH &&
                    transition.completed("wrong")?.reason == Rejection.RUN_MISMATCH,
                "mismatched transitions",
            )
            val dungeon = DungeonRun(phase = phase)
            verify((dungeon.advance() == null) == (phase == RunPhase.Active), "advance $phase")
        }

        val run = PartyRun()
        verify(run.join("leader") == null && run.join("member") == null, "forming membership")
        verify(run.join("member")?.reason == Rejection.INVALID_PARTY, "duplicate member")
        run.enter("leader")
        val runId = requireNotNull(run.runId)
        verify(run.join("late")?.reason == Rejection.INVALID_PARTY, "membership frozen")
        run.prepared(runId)
        run.arrived(runId, "leader")
        run.arrived(runId, "member")
        verify(run.started(runId) == null && run.completed(runId) == null, "complete state machine")
        val roundtripParty =
            Json.mapper.readValue(Json.mapper.writeValueAsBytes(run), PartyRun::class.java)
        verify(
            Json.mapper.writeValueAsString(roundtripParty) == Json.mapper.writeValueAsString(run),
            "party snapshot",
        )
        run.enter("leader")
        verify(run.runId != runId && run.phase == RunPhase.Preparing, "fresh run after completion")

        val dungeon = DungeonRun()
        val prepare =
            PrepareDungeon(
                runId,
                "party-${UUID.randomUUID().toString().replace("-", "")}",
                listOf("leader", "member"),
            )
        verify(PartyRun.validId(prepare.partyId, "party-"), "party ID")
        verify(dungeon.prepare(prepare) == null && dungeon.prepare(prepare) == null, "idempotent preparation")
        val preparedState = Json.mapper.writeValueAsString(dungeon)
        val wrongRun = prepare.copy(runId = "other")
        val wrongMembers = prepare.copy(members = listOf("other"))
        verify(
            dungeon.prepare(wrongRun)?.reason == Rejection.RUN_MISMATCH &&
                dungeon.prepare(wrongMembers)?.reason == Rejection.RUN_MISMATCH &&
                Json.mapper.writeValueAsString(dungeon) == preparedState,
            "preparation mismatch immutable",
        )
        dungeon.arrived("leader")
        verify(
            dungeon.phase == RunPhase.Entering && dungeon.advance()?.reason == Rejection.RUN_NOT_ACTIVE,
            "partial arrival",
        )
        dungeon.arrived("member")
        for (stage in 1..Rules.FINAL_STAGE)
            verify(dungeon.advance() == null && dungeon.stage == stage, "stage $stage")
        verify(
            dungeon.phase == RunPhase.Completed && dungeon.advance()?.reason == Rejection.RUN_NOT_ACTIVE,
            "final stage",
        )
        val roundtripDungeon =
            Json.mapper.readValue(Json.mapper.writeValueAsBytes(dungeon), DungeonRun::class.java)
        verify(
            Json.mapper.writeValueAsString(roundtripDungeon) == Json.mapper.writeValueAsString(dungeon),
            "dungeon snapshot",
        )
        verify(
            Json.mapper.writeValueAsString(run.view(prepare.partyId)).contains("\"phase\":\"Preparing\""),
            "enum name JSON",
        )
        verify(
            Json.mapper.writeValueAsString(PartyRun().view(prepare.partyId)).contains("\"runId\":null"),
            "nullable JSON",
        )
        println("Domain checks: $count/$count passed")
        return count
    }
}
