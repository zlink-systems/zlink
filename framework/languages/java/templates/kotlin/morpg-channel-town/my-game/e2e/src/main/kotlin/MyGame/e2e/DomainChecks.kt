package MyGame.e2e

import MyGame.*

object DomainChecks {
    fun run() {
        var count = 0
        fun verify(value: Boolean, name: String) {
            check(value) { "Domain: $name" }
            count++
        }

        verify(ChannelRules.validate(Names.MAXIMUM_INTEGER + 1) == Rejection.INTEGER_OUT_OF_RANGE, "channel above maximum")
        verify(ChannelRules.validate(Names.MAXIMUM_INTEGER) == Rejection.INVALID_CHANNEL, "channel maximum")
        verify(ChannelRules.validate(-Names.MAXIMUM_INTEGER - 1) == Rejection.INTEGER_OUT_OF_RANGE, "channel below minimum")
        verify(ChannelRules.validate(0) == Rejection.INVALID_CHANNEL, "channel zero")
        verify(ChannelRules.validate(1) == null && ChannelRules.validate(2) == null, "valid channels")
        verify(ChannelRules.validate(3) == Rejection.INVALID_CHANNEL, "channel above count")

        val runId = "run-" + "a".repeat(32)
        for (phase in RunPhase.entries) {
            for (action in listOf("enter", "prepared", "started", "completed")) {
                val sample =
                    PartyRun(
                        members = mutableListOf("leader", "member"),
                        phase = phase,
                        runId = if (phase == RunPhase.Forming) null else runId,
                    )
                val snapshot = Json.mapper.writeValueAsString(sample)
                val result =
                    when (action) {
                        "enter" -> sample.enter("leader")
                        "prepared" -> sample.prepared(runId)
                        "started" -> sample.started(runId)
                        else -> sample.completed(runId)
                    }
                val allowed =
                    when (action) {
                        "enter" -> true
                        "prepared" -> phase == RunPhase.Preparing || phase == RunPhase.Entering
                        "started" -> phase == RunPhase.Entering || phase == RunPhase.Active
                        else -> phase == RunPhase.Active || phase == RunPhase.Completed
                    }
                verify(
                    if (allowed) result == null
                    else result == Rejection.RUN_MISMATCH && Json.mapper.writeValueAsString(sample) == snapshot,
                    "transition table $phase/$action",
                )
            }
            val original =
                PartyRun(
                    members = mutableListOf("leader", "member"),
                    phase = phase,
                    runId = if (phase == RunPhase.Forming) null else runId,
                )
            val encoded = Json.mapper.writeValueAsString(original)
            verify(
                Json.mapper.writeValueAsString(
                    Json.mapper.readValue(encoded, PartyRun::class.java),
                ) == encoded,
                "snapshot $phase",
            )
        }

        val party = PartyRun(members = mutableListOf("leader", "member"))
        verify(Names.validId(party.partyId, "party-"), "party ID")
        verify(party.phase == RunPhase.Forming && party.runId == null, "forming")
        verify(party.changeChannel() == null, "forming channel")
        var before = Json.mapper.writeValueAsString(party)
        verify(party.enter("member") == Rejection.NOT_LEADER && before == Json.mapper.writeValueAsString(party), "nonleader unchanged")
        verify(party.started("unknown") == Rejection.RUN_MISMATCH, "forming start rejected")
        verify(party.completed("unknown") == Rejection.RUN_MISMATCH, "forming completion rejected")
        verify(party.prepared("unknown") == Rejection.RUN_MISMATCH, "forming preparation rejected")
        verify(party.enter("leader") == null && party.phase == RunPhase.Preparing, "prepare transition")
        val run = requireNotNull(party.runId)
        verify(Names.validId(run, "run-"), "run ID")
        verify(party.enter("leader") == null && party.runId == run, "preparing retry")
        verify(party.changeChannel() == Rejection.RUN_IN_PROGRESS, "preparing channel")
        party.leave("member")
        verify("member" in party.members, "preparing retains member")
        verify(party.started(run) == Rejection.RUN_MISMATCH, "preparing start rejected")
        verify(party.completed(run) == Rejection.RUN_MISMATCH, "preparing completion rejected")
        verify(party.prepared("other") == Rejection.RUN_MISMATCH, "wrong preparation")
        verify(party.prepared(run) == null && party.phase == RunPhase.Entering, "entering transition")
        verify(party.prepared(run) == null, "prepared retry")
        verify(party.enter("leader") == null && party.runId == run, "entering retry")
        verify(party.completed(run) == Rejection.RUN_MISMATCH, "entering completion rejected")
        verify(party.changeChannel() == Rejection.RUN_IN_PROGRESS, "entering channel")
        party.leave("member")
        verify(party.members.size == 2, "entering retains member")
        verify(party.started(run) == null && party.phase == RunPhase.Active, "active transition")
        verify(party.started(run) == null, "started retry")
        verify(party.enter("leader") == null && party.runId == run, "active enter unchanged")
        verify(party.prepared(run) == Rejection.RUN_MISMATCH, "active prepare rejected")
        verify(party.completed("other") == Rejection.RUN_MISMATCH, "wrong completion")
        verify(party.changeChannel() == Rejection.RUN_IN_PROGRESS, "active channel")
        party.leave("member")
        verify(party.members.size == 2, "active retains member")
        verify(party.completed(run) == null && party.phase == RunPhase.Completed, "completed transition")
        verify(party.completed(run) == null, "completed retry")
        verify(
            party.prepared(run) == Rejection.RUN_MISMATCH && party.started(run) == Rejection.RUN_MISMATCH,
            "completed rejects old transitions",
        )
        verify(party.changeChannel() == Rejection.RUN_IN_PROGRESS, "completed channel")
        party.leave("member")
        verify(party.members.size == 2, "completed retains member")
        val partySnapshot = Json.mapper.writeValueAsString(party)
        verify(
            Json.mapper.writeValueAsString(Json.mapper.readValue(partySnapshot, PartyRun::class.java)) == partySnapshot,
            "party snapshot",
        )
        verify(party.enter("leader") == null && party.runId != run && party.phase == RunPhase.Preparing, "new completed run")
        val forming = PartyRun(members = mutableListOf("leader"))
        forming.leave("leader")
        verify(forming.members.isEmpty(), "forming leave")

        val dungeon = DungeonState()
        val dungeonId = Names.dungeonId(run)
        val request = PrepareDungeon(run, party.partyId, "town-2", listOf("leader", "member"))
        verify(dungeon.advance() == Rejection.RUN_NOT_ACTIVE && dungeon.stage == 0L, "unprepared advance")
        verify(dungeon.prepare(dungeonId, request) == null, "prepare Dungeon")
        before = Json.mapper.writeValueAsString(dungeon)
        verify(dungeon.prepare(dungeonId, request) == null && before == Json.mapper.writeValueAsString(dungeon), "idempotent prepare")
        verify(
            dungeon.prepare(dungeonId, request.copy(runId = party.runId!!)) == Rejection.RUN_MISMATCH &&
                before == Json.mapper.writeValueAsString(dungeon),
            "wrong run unchanged",
        )
        verify(
            dungeon.prepare(dungeonId, request.copy(members = listOf("member", "leader"))) == Rejection.RUN_MISMATCH &&
                before == Json.mapper.writeValueAsString(dungeon),
            "wrong members unchanged",
        )
        verify(
            dungeon.prepare(dungeonId, request.copy(ownerSpotId = "town-1")) == Rejection.RUN_MISMATCH &&
                before == Json.mapper.writeValueAsString(dungeon),
            "wrong owner unchanged",
        )
        verify(dungeon.advance() == Rejection.RUN_NOT_ACTIVE && before == Json.mapper.writeValueAsString(dungeon), "entering advance unchanged")
        verify(!dungeon.accepts("other", dungeonId, JoinDungeon(dungeonId, run)), "nonmember admission")
        verify(!dungeon.accepts("leader", dungeonId, JoinDungeon(dungeonId, "other")), "wrong run admission")
        verify(dungeon.accepts("leader", dungeonId, JoinDungeon(dungeonId, run)), "member admission")
        verify(!dungeon.join("other") && dungeon.joined.isEmpty(), "unknown join unchanged")
        verify(!dungeon.join("leader") && dungeon.phase == RunPhase.Entering, "partial arrival")
        verify(!dungeon.join("leader") && dungeon.joined.size == 1, "duplicate arrival")
        verify(dungeon.join("member") && dungeon.phase == RunPhase.Active, "all arrive")
        verify(dungeon.advance() == null && dungeon.stage == 1L, "advance one")
        verify(dungeon.advance() == null && dungeon.stage == 2L, "advance two")
        verify(
            dungeon.advance() == null && dungeon.stage == Names.FINAL_STAGE && dungeon.phase == RunPhase.Completed,
            "final stage",
        )
        before = Json.mapper.writeValueAsString(dungeon)
        verify(dungeon.advance() == Rejection.RUN_NOT_ACTIVE && before == Json.mapper.writeValueAsString(dungeon), "completed advance unchanged")
        verify(
            Json.mapper.writeValueAsString(Json.mapper.readValue(before, DungeonState::class.java)) == before,
            "Dungeon snapshot",
        )
        dungeon.stage = Names.MAXIMUM_INTEGER
        before = Json.mapper.writeValueAsString(dungeon)
        verify(
            dungeon.advance() == Rejection.INTEGER_OUT_OF_RANGE && Json.mapper.writeValueAsString(dungeon) == before,
            "integer boundary unchanged",
        )
        dungeon.stage = Names.MAXIMUM_INTEGER + 1
        verify(dungeon.advance() == Rejection.INTEGER_OUT_OF_RANGE, "integer above bound")

        verify(Json.mapper.writeValueAsString(RunPhase.Active) == "\"Active\"", "enum name")
        verify(
            Json.mapper.writeValueAsString(PartyView("party-id", emptyList(), RunPhase.Active, null)) ==
                "{\"partyId\":\"party-id\",\"members\":[],\"phase\":\"Active\",\"runId\":null}",
            "camelCase JSON and nullable run",
        )
        verify(Json.mapper.writeValueAsString(PartyRun()).contains("\"runId\":null"), "nullable run")
        verify(ActionRejected(Rejection.RUN_NOT_ACTIVE).retryAfterMs == 0, "rejection time")
        println("Domain checks: $count/$count passed")
    }
}
