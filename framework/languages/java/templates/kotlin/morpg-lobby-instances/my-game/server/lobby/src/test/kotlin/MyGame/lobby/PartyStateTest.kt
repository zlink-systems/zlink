package MyGame.lobby

import MyGame.*
import MyGame.lobby.domain.PartyState
import org.junit.jupiter.api.Assertions.*
import org.junit.jupiter.api.Test

class PartyStateTest {
    @Test
    fun admissionEnforcesIdentityAndCapacity() {
        val state = PartyState()
        assertFalse(state.join("p1", PartyJoin("", true)))
        assertFalse(state.join("p1", PartyJoin("alpha", false)))
        assertTrue(state.join("p1", PartyJoin("alpha", true)))
        assertFalse(state.join("p1", PartyJoin("alpha", false)))
        assertFalse(state.join("p2", PartyJoin("beta", false)))
        assertFalse(state.join("p2", PartyJoin("alpha", true)))
        for (n in 2..Rules.MAX_MEMBERS) assertTrue(state.join("p$n", PartyJoin("alpha", false)))
        assertFalse(state.join("overflow", PartyJoin("alpha", false)))
    }

    @Test
    fun entryRequiresLeaderAndEveryMemberReady() {
        val state = PartyState()
        state.join("p1", PartyJoin("alpha", true))
        state.join("p2", PartyJoin("alpha", false))
        assertEquals(Rules.NOT_LEADER, state.entryRejection("p2"))
        assertEquals(Rules.NOT_READY, state.entryRejection("p1"))
        state.ready.add("p1")
        assertEquals(Rules.NOT_READY, state.entryRejection("p1"))
        state.ready.add("p2")
        assertNull(state.entryRejection("p1"))
        assertEquals(listOf(true, true), state.view().ready)
    }

    @Test
    fun transferPreservesOrderedRosterAndReadiness() {
        val state = PartyState()
        state.join("p1", PartyJoin("alpha", true))
        state.join("p2", PartyJoin("alpha", false))
        state.ready.add("p2")
        val restored =
            Json.mapper.readValue(Json.mapper.writeValueAsBytes(state), PartyState::class.java)
        assertEquals(state.view(), restored.view())
        restored.leave("p1")
        assertEquals(listOf("p2"), restored.view().members)
        assertNull(restored.entryRejection("p2"))
        assertEquals(listOf("p1", "p2"), state.view().members)
    }
    @Test
    fun runBoundaries() {
        var count = 0
        fun verify(value: Boolean, name: String) { assertTrue(value, name); count++ }
        fun snap(value: Any) = Json.mapper.writeValueAsString(value)
        fun party(phase: RunPhase = RunPhase.Forming) = PartyState().also {
            it.partyId = Names.newPartyId(); it.members.addAll(listOf("leader", "member")); it.ready.addAll(it.members)
            it.phase = phase; it.runId = if (phase == RunPhase.Forming) null else Names.newRunId()
        }
        verify(Regex("party-[0-9a-f]{32}").matches(Names.newPartyId()), "party ID")
        verify(Regex("run-[0-9a-f]{32}").matches(Names.newRunId()), "run ID")
        verify(Rules.MAXIMUM_INTEGER == 9_007_199_254_740_991L && Rules.FINAL_STAGE == 3, "bounds")
        for (phase in RunPhase.entries) {
            var p = party(phase); var before = snap(p)
            verify(p.enter("member") == Rules.NOT_LEADER && snap(p) == before, "non-leader unchanged")
            p.ready.remove("member"); before = snap(p)
            verify(p.enter("leader") == Rules.NOT_READY && snap(p) == before, "not-ready unchanged")
            p.ready.add("member"); var old = p.runId
            verify(p.enter("leader") == null, "leader")
            verify(if (phase in listOf(RunPhase.Forming, RunPhase.Completed)) p.phase == RunPhase.Preparing && p.runId != old else p.phase == phase && p.runId == old, "entry")
            old = p.runId; verify(p.enter("leader") == null && p.runId == old, "repeated entry")
            p = party(phase); before = snap(p)
            verify(p.prepared("wrong") == Rules.RUN_MISMATCH && snap(p) == before, "prepared mismatch")
            verify((p.prepared(p.runId ?: "") == null) == (phase in listOf(RunPhase.Preparing, RunPhase.Entering)), "prepared table")
            p = party(phase); before = snap(p)
            verify(p.started("wrong") == Rules.RUN_MISMATCH && snap(p) == before, "started mismatch")
            verify((p.started(p.runId ?: "") == null) == (phase in listOf(RunPhase.Entering, RunPhase.Active)), "started table")
            p = party(phase); before = snap(p)
            verify(p.completed("wrong") == Rules.RUN_MISMATCH && snap(p) == before, "completed mismatch")
            verify((p.completed(p.runId ?: "") == null) == (phase in listOf(RunPhase.Active, RunPhase.Completed)), "completed table")
            p = party(phase); p.leave("member")
            verify(("member" in p.members) == (phase != RunPhase.Forming), "leave")
            verify(snap(Json.mapper.readValue(snap(p), PartyState::class.java)) == snap(p), "party snapshot")
            val d = MyGame.instance.domain.DungeonState().also { it.phase = phase }; before = snap(d)
            verify((d.advance() == null) == (phase == RunPhase.Active), "advance table")
            verify(phase == RunPhase.Active || snap(d) == before, "rejected advance unchanged")
        }
        var d = MyGame.instance.domain.DungeonState()
        val request = PrepareDungeon(Names.newRunId(), Names.newPartyId(), listOf("leader", "member"))
        verify(d.prepare(request) == null, "prepare"); var snapshot = snap(d)
        verify(d.prepare(request) == null && snap(d) == snapshot, "prepare idempotence")
        verify(d.prepare(request.copy(runId = Names.newRunId())) == Rules.RUN_MISMATCH && snap(d) == snapshot, "run mismatch")
        verify(d.prepare(request.copy(members = listOf("leader"))) == Rules.RUN_MISMATCH && snap(d) == snapshot, "roster mismatch")
        verify(d.prepare(request.copy(partyId = Names.newPartyId())) == Rules.RUN_MISMATCH && snap(d) == snapshot, "party mismatch")
        verify(!d.admit("outsider", JoinDungeon(Names.dungeonId(request.runId), request.runId), Names.dungeonId(request.runId)), "admit roster")
        verify(!d.join("outsider") && snap(d) == snapshot, "outsider unchanged")
        verify(!d.join("leader") && d.phase == RunPhase.Entering, "partial arrival")
        verify(!d.join("leader") && d.phase == RunPhase.Entering, "duplicate arrival")
        verify(d.join("member") && d.phase == RunPhase.Active, "all arrived")
        verify(!d.join("member") && d.phase == RunPhase.Active, "started idempotence")
        for (stage in 1L..Rules.FINAL_STAGE.toLong()) verify(d.advance() == null && d.stage == stage && d.phase == (if (stage == Rules.FINAL_STAGE.toLong()) RunPhase.Completed else RunPhase.Active), "stage")
        snapshot = snap(d); verify(d.advance() == Rules.RUN_NOT_ACTIVE && snap(d) == snapshot, "completed unchanged")
        verify(snap(Json.mapper.readValue(snapshot, MyGame.instance.domain.DungeonState::class.java)) == snapshot, "dungeon snapshot")
        for (stage in listOf(-1L, Rules.MAXIMUM_INTEGER, Rules.MAXIMUM_INTEGER + 1, Long.MAX_VALUE)) {
            d = MyGame.instance.domain.DungeonState().also { it.stage = stage; it.phase = RunPhase.Active }; snapshot = snap(d)
            verify(d.advance() == Rules.INTEGER_OUT_OF_RANGE && snap(d) == snapshot, "integer boundary")
        }
        val arrivals = party(RunPhase.Entering); var arrivalSnapshot = snap(arrivals)
        verify(arrivals.arrived("wrong", "member") == Rules.RUN_MISMATCH && snap(arrivals) == arrivalSnapshot, "arrival run mismatch")
        verify(arrivals.arrived(requireNotNull(arrivals.runId), "outsider") == Rules.RUN_MISMATCH && snap(arrivals) == arrivalSnapshot, "arrival roster mismatch")
        verify(arrivals.arrived(requireNotNull(arrivals.runId), "member") == null && "member" in arrivals.entered, "arrival recorded")
        arrivalSnapshot = snap(arrivals)
        verify(arrivals.arrived(requireNotNull(arrivals.runId), "member") == null && snap(arrivals) == arrivalSnapshot, "arrival idempotent")
        verify(snap(Json.mapper.readValue(arrivalSnapshot, PartyState::class.java)) == arrivalSnapshot, "arrival snapshot")
        arrivals.phase = RunPhase.Completed
        verify(arrivals.enter("leader") == null && arrivals.entered.isEmpty(), "new run clears arrivals")
        val view = snap(party().view())
        verify(view.contains("\"runId\":null") && view.contains("\"phase\":\"Forming\""), "enum null JSON")
        println("Domain checks: $count/$count passed")
    }
}
