package MyGame.e2e;

import MyGame.*;
import MyGame.field.FieldPositionDomain;
import MyGame.field.PartyRun;
import MyGame.instance.DungeonRun;

import java.util.*;
import java.util.concurrent.atomic.AtomicLong;

final class DomainChecks {
    private DomainChecks() {}

    static int run() throws Exception {
        int[] count = {0};
        var check = (java.util.function.BiConsumer<Boolean, String>) (condition, message) -> {
            if (!condition) throw new IllegalStateException("Domain: " + message);
            count[0]++;
        };
        var now = new AtomicLong(1000);
        var domain = new FieldPositionDomain("field-1", now::get);
        domain.addPlayer("p", new EnterField(50, 0));
        var invalidMoves = List.of(
                new MoveRequest(Double.NaN, 1),
                new MoveRequest(Double.POSITIVE_INFINITY, 1),
                new MoveRequest(Double.NEGATIVE_INFINITY, 1),
                new MoveRequest(10.01, 1),
                new MoveRequest(0, Rules.MAX_SAFE_INTEGER + 1),
                new MoveRequest(0, -1),
                new MoveRequest(0, 0));
        var invalidReasons = List.of(
                Rejection.NON_FINITE,
                Rejection.NON_FINITE,
                Rejection.NON_FINITE,
                Rejection.STEP_TOO_LARGE,
                Rejection.INTEGER_OUT_OF_RANGE,
                Rejection.STALE_SEQUENCE,
                Rejection.STALE_SEQUENCE);
        for (int i = 0; i < invalidMoves.size(); i++) {
            var before = domain.state("p");
            var rejected = domain.move("p", invalidMoves.get(i));
            check.accept(rejected.rejection() != null
                            && rejected.rejection().reason().equals(invalidReasons.get(i))
                            && rejected.rejection().retryAfterMs() == 0,
                    "movement rejection " + invalidReasons.get(i));
            check.accept(domain.state("p").equals(before), "movement rejection changed state");
        }
        check.accept(domain.move("p", new MoveRequest(10, 1)).accepted(), "first move budget");
        var state = domain.state("p");
        var budget = domain.move("p", new MoveRequest(10, 2));
        check.accept(budget.rejection() != null
                        && budget.rejection().reason().equals(Rejection.MOVE_BUDGET)
                        && budget.rejection().retryAfterMs() == 200,
                "budget retry");
        check.accept(domain.state("p").equals(state), "budget rejection changed state");
        now.addAndGet(budget.rejection().retryAfterMs());
        check.accept(domain.move("p", new MoveRequest(10, 2)).accepted(), "retry after server delay");
        now.addAndGet(25);
        var snapshot = Json.MAPPER.readValue(
                Json.MAPPER.writeValueAsBytes(domain.capture()),
                FieldPositionDomain.FieldPositionSnapshot.class);
        var restored = new FieldPositionDomain("field-1", () -> now.get() + 10_000);
        restored.restore(snapshot);
        check.accept(restored.state("p").equals(domain.state("p")), "relative time snapshot");
        check.accept(restored.move("p", new MoveRequest(0, Rules.MAX_SAFE_INTEGER)).accepted(),
                "safe integer upper bound");
        check.accept(restored.move("p", new MoveRequest(0, Rules.MAX_SAFE_INTEGER + 1)).rejection()
                        .reason().equals(Rejection.INTEGER_OUT_OF_RANGE),
                "range before stale");
        check.accept(restored.move("p", new MoveRequest(0, Rules.MAX_SAFE_INTEGER)).rejection()
                        .reason().equals(Rejection.STALE_SEQUENCE),
                "maximum sequence stale");
        domain.addPlayer("edge", new EnterField(0, 0));
        check.accept(domain.move("edge", new MoveRequest(-1, 1)).rejection()
                        .reason().equals(Rejection.OUTSIDE_WORLD),
                "world minimum");
        var last = new FieldPositionDomain("field-4", now::get);
        last.addPlayer("edge", new EnterField(399, 0));
        check.accept(last.move("edge", new MoveRequest(1, 1)).rejection()
                        .reason().equals(Rejection.OUTSIDE_WORLD),
                "world maximum");
        last.addPlayer("fraction", new EnterField(399.5, 0));
        check.accept(last.move("fraction", new MoveRequest(0, 1)).accepted(),
                "fractional world edge");
        check.accept(domain.queueArrival("x", new EnterField(100, 0)).reason()
                        .equals(Rejection.OUTSIDE_WORLD),
                "arrival field");
        check.accept(domain.queueArrival("x", new EnterField(1, Rules.MAX_SAFE_INTEGER + 1))
                        .reason().equals(Rejection.INTEGER_OUT_OF_RANGE),
                "arrival sequence");
        check.accept(domain.queueArrival("x", new EnterField(1, 0, 201)).reason()
                        .equals(Rejection.INTEGER_OUT_OF_RANGE),
                "arrival remaining time");

        for (var phase : RunPhase.values()) {
            var party = new PartyRun();
            party.members = new ArrayList<>(List.of("leader", "member"));
            party.phase = phase;
            party.runId = phase == RunPhase.Forming ? null : "run-" + "a".repeat(32);
            var before = Json.MAPPER.writeValueAsString(party);
            var nonleader = party.enter("member");
            check.accept(nonleader != null
                            && nonleader.reason().equals(Rejection.NOT_LEADER)
                            && Json.MAPPER.writeValueAsString(party).equals(before),
                    "nonleader immutable in " + phase);
            var oldRun = party.runId;
            check.accept(party.enter("leader") == null, "leader entry " + phase);
            check.accept(PartyRun.validId(party.runId, "run-"), "run ID");
            check.accept((phase == RunPhase.Forming || phase == RunPhase.Completed)
                            ? !Objects.equals(party.runId, oldRun) && party.phase == RunPhase.Preparing
                            : Objects.equals(party.runId, oldRun) && party.phase == phase,
                    "entry transition " + phase);
            var leaving = new PartyRun();
            leaving.members = new ArrayList<>(List.of("leader", "member"));
            leaving.phase = phase;
            leaving.leave("member");
            check.accept(leaving.members.size() == (phase == RunPhase.Forming ? 1 : 2),
                    "leave " + phase);
            var transition = new PartyRun();
            transition.members = new ArrayList<>(List.of("leader"));
            transition.phase = phase;
            transition.runId = "run";
            check.accept((transition.prepared("run") == null)
                            == (phase == RunPhase.Preparing || phase == RunPhase.Entering),
                    "prepared transition " + phase);
            transition.phase = phase;
            transition.entered.add("leader");
            check.accept((transition.started("run") == null)
                            == (phase == RunPhase.Entering || phase == RunPhase.Active),
                    "started transition " + phase);
            transition.phase = phase;
            check.accept((transition.completed("run") == null)
                            == (phase == RunPhase.Active || phase == RunPhase.Completed),
                    "completed transition " + phase);
            transition.phase = phase;
            check.accept(transition.prepared("wrong").reason().equals(Rejection.RUN_MISMATCH)
                            && transition.started("wrong").reason().equals(Rejection.RUN_MISMATCH)
                            && transition.completed("wrong").reason().equals(Rejection.RUN_MISMATCH),
                    "mismatched transitions");
            var dungeon = new DungeonRun();
            dungeon.phase = phase;
            check.accept((dungeon.advance() == null) == (phase == RunPhase.Active),
                    "advance " + phase);
        }

        var run = new PartyRun();
        check.accept(run.join("leader") == null && run.join("member") == null, "forming membership");
        check.accept(run.join("member").reason().equals(Rejection.INVALID_PARTY), "duplicate member");
        run.enter("leader");
        String runId = run.runId;
        check.accept(run.join("late").reason().equals(Rejection.INVALID_PARTY), "membership frozen");
        run.prepared(runId);
        run.arrived(runId, "leader");
        run.arrived(runId, "member");
        check.accept(run.started(runId) == null && run.completed(runId) == null,
                "complete state machine");
        var roundtripParty = Json.MAPPER.readValue(
                Json.MAPPER.writeValueAsBytes(run), PartyRun.class);
        check.accept(Json.MAPPER.writeValueAsString(roundtripParty)
                        .equals(Json.MAPPER.writeValueAsString(run)),
                "party snapshot");
        run.enter("leader");
        check.accept(!run.runId.equals(runId) && run.phase == RunPhase.Preparing,
                "fresh run after completion");

        var dungeon = new DungeonRun();
        var prepare = new PrepareDungeon(runId,
                "party-" + UUID.randomUUID().toString().replace("-", ""),
                List.of("leader", "member"));
        check.accept(PartyRun.validId(prepare.partyId(), "party-"), "party ID");
        check.accept(dungeon.prepare(prepare) == null && dungeon.prepare(prepare) == null,
                "idempotent preparation");
        var preparedState = Json.MAPPER.writeValueAsString(dungeon);
        var wrongRun = new PrepareDungeon("other", prepare.partyId(), prepare.members());
        var wrongMembers = new PrepareDungeon(prepare.runId(), prepare.partyId(), List.of("other"));
        check.accept(dungeon.prepare(wrongRun).reason().equals(Rejection.RUN_MISMATCH)
                        && dungeon.prepare(wrongMembers).reason().equals(Rejection.RUN_MISMATCH)
                        && Json.MAPPER.writeValueAsString(dungeon).equals(preparedState),
                "preparation mismatch immutable");
        dungeon.arrived("leader");
        check.accept(dungeon.phase == RunPhase.Entering
                        && dungeon.advance().reason().equals(Rejection.RUN_NOT_ACTIVE),
                "partial arrival");
        dungeon.arrived("member");
        for (int stage = 1; stage <= Rules.FINAL_STAGE; stage++)
            check.accept(dungeon.advance() == null && dungeon.stage == stage, "stage " + stage);
        check.accept(dungeon.phase == RunPhase.Completed
                        && dungeon.advance().reason().equals(Rejection.RUN_NOT_ACTIVE),
                "final stage");
        var roundtripDungeon = Json.MAPPER.readValue(
                Json.MAPPER.writeValueAsBytes(dungeon), DungeonRun.class);
        check.accept(Json.MAPPER.writeValueAsString(roundtripDungeon)
                        .equals(Json.MAPPER.writeValueAsString(dungeon)),
                "dungeon snapshot");
        check.accept(Json.MAPPER.writeValueAsString(run.view(prepare.partyId()))
                        .contains("\"phase\":\"Preparing\""),
                "enum name JSON");
        check.accept(Json.MAPPER.writeValueAsString(new PartyRun().view(prepare.partyId()))
                        .contains("\"runId\":null"),
                "nullable JSON");
        System.out.println("Domain checks: " + count[0] + "/" + count[0] + " passed");
        return count[0];
    }
}
