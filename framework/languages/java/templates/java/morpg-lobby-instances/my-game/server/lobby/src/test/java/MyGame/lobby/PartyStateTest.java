package MyGame.lobby;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.util.List;

class PartyStateTest {
    @Test
    void admissionEnforcesPartyIdentityAndCapacity() {
        var s = new PartyState();
        assertFalse(s.join("p1", new PartyJoin("", true)));
        assertFalse(s.join("p1", new PartyJoin("alpha", false)));
        assertTrue(s.join("p1", new PartyJoin("alpha", true)));
        assertFalse(s.join("p1", new PartyJoin("alpha", false)));
        assertFalse(s.join("p2", new PartyJoin("beta", false)));
        assertFalse(s.join("p2", new PartyJoin("alpha", true)));
        for (int i = 2; i <= Rules.MAX_MEMBERS; i++)
            assertTrue(s.join("p" + i, new PartyJoin("alpha", false)));
        assertFalse(s.join("overflow", new PartyJoin("alpha", false)));
    }

    @Test
    void entryRequiresLeaderAndEveryMemberReady() {
        var s = new PartyState();
        s.join("p1", new PartyJoin("alpha", true));
        s.join("p2", new PartyJoin("alpha", false));
        assertEquals(Rules.NOT_LEADER, s.entryRejection("p2"));
        assertEquals(Rules.NOT_READY, s.entryRejection("p1"));
        s.ready.add("p1");
        assertEquals(Rules.NOT_READY, s.entryRejection("p1"));
        s.ready.add("p2");
        assertNull(s.entryRejection("p1"));
        assertEquals(List.of(true, true), s.view().ready());
    }

    @Test
    void relocationPreservesOrderedRosterAndReadiness() throws Exception {
        var s = new PartyState();
        s.join("p1", new PartyJoin("alpha", true));
        s.join("p2", new PartyJoin("alpha", false));
        s.ready.add("p2");
        var restored = Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(s), PartyState.class);
        assertEquals(s.view(), restored.view());
        restored.leave("p1");
        assertEquals(List.of("p2"), restored.view().members());
        assertNull(restored.entryRejection("p2"));
        assertEquals(List.of("p1", "p2"), s.view().members());
    }
    @Test
    void runBoundaries() throws Exception {
        int[] count = {0};
        java.util.function.BiConsumer<Boolean, String> check = (v, name) -> { assertTrue(v, name); count[0]++; };
        java.util.function.Function<Object, String> snap = value -> {
            try { return Json.MAPPER.writeValueAsString(value); } catch (Exception e) { throw new IllegalStateException(e); }
        };
        java.util.function.Function<RunPhase, PartyState> party = phase -> {
            var p = new PartyState(); p.partyId = Names.newPartyId(); p.members.addAll(List.of("leader", "member")); p.ready.addAll(p.members); p.phase = phase;
            p.runId = phase == RunPhase.Forming ? null : Names.newRunId(); return p;
        };
        check.accept(Names.newPartyId().matches("party-[0-9a-f]{32}"), "party ID");
        check.accept(Names.newRunId().matches("run-[0-9a-f]{32}"), "run ID");
        check.accept(Rules.MAXIMUM_INTEGER == 9_007_199_254_740_991L && Rules.FINAL_STAGE == 3, "bounds");
        for (var phase : RunPhase.values()) {
            var p = party.apply(phase); var before = snap.apply(p);
            check.accept(Rules.NOT_LEADER.equals(p.enter("member")) && snap.apply(p).equals(before), "non-leader unchanged");
            p.ready.remove("member"); before = snap.apply(p);
            check.accept(Rules.NOT_READY.equals(p.enter("leader")) && snap.apply(p).equals(before), "not-ready unchanged");
            p.ready.add("member"); var old = p.runId;
            check.accept(p.enter("leader") == null, "leader");
            check.accept(phase == RunPhase.Forming || phase == RunPhase.Completed ? p.phase == RunPhase.Preparing && !java.util.Objects.equals(p.runId, old) : p.phase == phase && java.util.Objects.equals(p.runId, old), "entry");
            old = p.runId; check.accept(p.enter("leader") == null && p.runId.equals(old), "repeated entry");
            p = party.apply(phase); before = snap.apply(p);
            check.accept(Rules.RUN_MISMATCH.equals(p.prepared("wrong")) && snap.apply(p).equals(before), "prepared mismatch");
            check.accept((p.prepared(p.runId == null ? "" : p.runId) == null) == (phase == RunPhase.Preparing || phase == RunPhase.Entering), "prepared table");
            p = party.apply(phase); before = snap.apply(p);
            check.accept(Rules.RUN_MISMATCH.equals(p.started("wrong")) && snap.apply(p).equals(before), "started mismatch");
            check.accept((p.started(p.runId == null ? "" : p.runId) == null) == (phase == RunPhase.Entering || phase == RunPhase.Active), "started table");
            p = party.apply(phase); before = snap.apply(p);
            check.accept(Rules.RUN_MISMATCH.equals(p.completed("wrong")) && snap.apply(p).equals(before), "completed mismatch");
            check.accept((p.completed(p.runId == null ? "" : p.runId) == null) == (phase == RunPhase.Active || phase == RunPhase.Completed), "completed table");
            p = party.apply(phase); p.leave("member");
            check.accept(p.members.contains("member") == (phase != RunPhase.Forming), "leave");
            check.accept(snap.apply(Json.MAPPER.readValue(snap.apply(p), PartyState.class)).equals(snap.apply(p)), "party snapshot");
            var d = new MyGame.instance.DungeonState(); d.phase = phase; before = snap.apply(d);
            check.accept((d.advance() == null) == (phase == RunPhase.Active), "advance table");
            check.accept(phase == RunPhase.Active || snap.apply(d).equals(before), "rejected advance unchanged");
        }
        var d = new MyGame.instance.DungeonState(); var request = new PrepareDungeon(Names.newRunId(), Names.newPartyId(), List.of("leader", "member"));
        check.accept(d.prepare(request) == null, "prepare"); var snapshot = snap.apply(d);
        check.accept(d.prepare(request) == null && snap.apply(d).equals(snapshot), "prepare idempotence");
        check.accept(Rules.RUN_MISMATCH.equals(d.prepare(new PrepareDungeon(Names.newRunId(), request.partyId(), request.members()))) && snap.apply(d).equals(snapshot), "run mismatch");
        check.accept(Rules.RUN_MISMATCH.equals(d.prepare(new PrepareDungeon(request.runId(), request.partyId(), List.of("leader")))) && snap.apply(d).equals(snapshot), "roster mismatch");
        check.accept(Rules.RUN_MISMATCH.equals(d.prepare(new PrepareDungeon(request.runId(), Names.newPartyId(), request.members()))) && snap.apply(d).equals(snapshot), "party mismatch");
        check.accept(!d.admit("outsider", new JoinDungeon(Names.dungeonId(request.runId()), request.runId()), Names.dungeonId(request.runId())), "admit roster");
        check.accept(!d.join("outsider") && snap.apply(d).equals(snapshot), "outsider unchanged");
        check.accept(!d.join("leader") && d.phase == RunPhase.Entering, "partial arrival");
        check.accept(!d.join("leader") && d.phase == RunPhase.Entering, "duplicate arrival");
        check.accept(d.join("member") && d.phase == RunPhase.Active, "all arrived");
        check.accept(!d.join("member") && d.phase == RunPhase.Active, "started idempotence");
        for (long stage = 1; stage <= Rules.FINAL_STAGE; stage++) check.accept(d.advance() == null && d.stage == stage && d.phase == (stage == Rules.FINAL_STAGE ? RunPhase.Completed : RunPhase.Active), "stage");
        snapshot = snap.apply(d); check.accept(Rules.RUN_NOT_ACTIVE.equals(d.advance()) && snap.apply(d).equals(snapshot), "completed unchanged");
        check.accept(snap.apply(Json.MAPPER.readValue(snapshot, MyGame.instance.DungeonState.class)).equals(snapshot), "dungeon snapshot");
        for (long stage : new long[] {-1, Rules.MAXIMUM_INTEGER, Rules.MAXIMUM_INTEGER + 1, Long.MAX_VALUE}) {
            d = new MyGame.instance.DungeonState(); d.stage = stage; d.phase = RunPhase.Active; snapshot = snap.apply(d);
            check.accept(Rules.INTEGER_OUT_OF_RANGE.equals(d.advance()) && snap.apply(d).equals(snapshot), "integer boundary");
        }
        var arrivals = party.apply(RunPhase.Entering); var arrivalSnapshot = snap.apply(arrivals);
        check.accept(Rules.RUN_MISMATCH.equals(arrivals.arrived("wrong", "member")) && snap.apply(arrivals).equals(arrivalSnapshot), "arrival run mismatch");
        check.accept(Rules.RUN_MISMATCH.equals(arrivals.arrived(arrivals.runId, "outsider")) && snap.apply(arrivals).equals(arrivalSnapshot), "arrival roster mismatch");
        check.accept(arrivals.arrived(arrivals.runId, "member") == null && arrivals.entered.contains("member"), "arrival recorded");
        arrivalSnapshot = snap.apply(arrivals);
        check.accept(arrivals.arrived(arrivals.runId, "member") == null && snap.apply(arrivals).equals(arrivalSnapshot), "arrival idempotent");
        check.accept(snap.apply(Json.MAPPER.readValue(arrivalSnapshot, PartyState.class)).equals(arrivalSnapshot), "arrival snapshot");
        arrivals.phase = RunPhase.Completed;
        check.accept(arrivals.enter("leader") == null && arrivals.entered.isEmpty(), "new run clears arrivals");
        var view = snap.apply(party.apply(RunPhase.Forming).view());
        check.accept(view.contains("\"runId\":null") && view.contains("\"phase\":\"Forming\""), "enum null JSON");
        System.out.println("Domain checks: " + count[0] + "/" + count[0] + " passed");
    }
}
