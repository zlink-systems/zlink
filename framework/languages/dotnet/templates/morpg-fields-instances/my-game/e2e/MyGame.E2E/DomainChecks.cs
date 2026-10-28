using System.Text.Json;
using MyGame.Field;
using MyGame.Instance;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class DomainChecks
{
    public static void Run()
    {
        var count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidDataException("Domain: " + message);
            count++;
        }
        long now = 1000;
        var domain = new FieldPositionDomain("field-1", () => now);
        domain.AddPlayer("p", new(50, 0));
        foreach (var (request, reason) in new[] {
            (new MoveRequest(double.NaN, 1), "non-finite"),
            (new MoveRequest(double.PositiveInfinity, 1), "non-finite"),
            (new MoveRequest(double.NegativeInfinity, 1), "non-finite"),
            (new MoveRequest(10.01, 1), "step-too-large"),
            (new MoveRequest(0, GameNames.MaxSafeInteger + 1), "integer-out-of-range"),
            (new MoveRequest(0, -1), "stale-sequence"),
            (new MoveRequest(0, 0), "stale-sequence") })
        {
            var before = domain.State("p");
            var rejected = domain.Move("p", request);
            Check(rejected.Rejection?.Reason == reason && rejected.Rejection.RetryAfterMs == 0,
                "movement rejection " + reason);
            Check(domain.State("p") == before, "movement rejection changed state");
        }
        Check(domain.Move("p", new(10, 1)).Accepted, "first move budget");
        var state = domain.State("p");
        var budget = domain.Move("p", new(10, 2));
        Check(budget.Rejection?.Reason == "move-budget" && budget.Rejection.RetryAfterMs == 200,
            "budget retry");
        Check(domain.State("p") == state, "budget rejection changed state");
        now += budget.Rejection!.RetryAfterMs;
        Check(domain.Move("p", new(10, 2)).Accepted, "retry after server delay");
        now += 25;
        var snapshot = JsonSerializer.Deserialize<FieldPositionSnapshot>(JsonSerializer.Serialize(domain.Capture()))!;
        var restored = new FieldPositionDomain("field-1", () => now + 10_000);
        restored.Restore(snapshot);
        Check(restored.State("p") == domain.State("p"), "relative time snapshot");
        Check(restored.Move("p", new(0, GameNames.MaxSafeInteger)).Accepted, "safe integer upper bound");
        Check(restored.Move("p", new(0, GameNames.MaxSafeInteger + 1)).Rejection?.Reason == "integer-out-of-range",
            "range before stale");
        Check(restored.Move("p", new(0, GameNames.MaxSafeInteger)).Rejection?.Reason == "stale-sequence",
            "maximum sequence stale");
        domain.AddPlayer("edge", new(0, 0));
        Check(domain.Move("edge", new(-1, 1)).Rejection?.Reason == "outside-world", "world minimum");
        var last = new FieldPositionDomain("field-4", () => now);
        last.AddPlayer("edge", new(399, 0));
        Check(last.Move("edge", new(1, 1)).Rejection?.Reason == "outside-world", "world maximum");
        last.AddPlayer("fraction", new(399.5, 0));
        Check(last.Move("fraction", new(0, 1)).Accepted, "fractional world edge");
        Check(domain.QueueArrival("x", new(100, 0))?.Reason == "outside-world", "arrival field");
        Check(domain.QueueArrival("x", new(1, GameNames.MaxSafeInteger + 1))?.Reason == "integer-out-of-range", "arrival sequence");
        Check(domain.QueueArrival("x", new(1, 0, 201))?.Reason == "integer-out-of-range", "arrival remaining time");
        foreach (var phase in Enum.GetValues<RunPhase>())
        {
            var party = new PartyRun { Members = ["leader", "member"], Phase = phase,
                RunId = phase == RunPhase.Forming ? null : "run-" + new string('a', 32) };
            var before = JsonSerializer.Serialize(party);
            Check(party.Enter("member")?.Reason == "not-leader" && JsonSerializer.Serialize(party) == before,
                "nonleader immutable in " + phase);
            var oldRun = party.RunId;
            Check(party.Enter("leader") is null, "leader entry " + phase);
            Check(PartyRun.ValidId(party.RunId, "run-"), "run ID");
            Check(phase is RunPhase.Forming or RunPhase.Completed ? party.RunId != oldRun
                && party.Phase == RunPhase.Preparing : party.RunId == oldRun && party.Phase == phase,
                "entry transition " + phase);
            var leaving = new PartyRun { Members = ["leader", "member"], Phase = phase };
            leaving.Leave("member");
            Check(leaving.Members.Count == (phase == RunPhase.Forming ? 1 : 2), "leave " + phase);
            var transition = new PartyRun { Members = ["leader"], Phase = phase, RunId = "run" };
            Check((transition.Prepared("run") is null) == (phase is RunPhase.Preparing or RunPhase.Entering),
                "prepared transition " + phase);
            transition.Phase = phase;
            transition.Entered.Add("leader");
            Check((transition.Started("run") is null) == (phase is RunPhase.Entering or RunPhase.Active),
                "started transition " + phase);
            transition.Phase = phase;
            Check((transition.Completed("run") is null) == (phase is RunPhase.Active or RunPhase.Completed),
                "completed transition " + phase);
            transition.Phase = phase;
            Check(transition.Prepared("wrong")?.Reason == "run-mismatch"
                && transition.Started("wrong")?.Reason == "run-mismatch"
                && transition.Completed("wrong")?.Reason == "run-mismatch", "mismatched transitions");
            var dungeon = new DungeonRun { Phase = phase };
            Check((dungeon.Advance() is null) == (phase == RunPhase.Active), "advance " + phase);
        }
        var run = new PartyRun();
        Check(run.Join("leader") is null && run.Join("member") is null, "forming membership");
        Check(run.Join("member")?.Reason == "invalid-party", "duplicate member");
        run.Enter("leader");
        var id = run.RunId!;
        Check(run.Join("late")?.Reason == "invalid-party", "membership frozen");
        run.Prepared(id);
        run.Arrived(id, "leader"); run.Arrived(id, "member");
        Check(run.Started(id) is null && run.Completed(id) is null, "complete state machine");
        var roundtrip = JsonSerializer.Deserialize<PartyRun>(JsonSerializer.Serialize(run))!;
        Check(JsonSerializer.Serialize(roundtrip) == JsonSerializer.Serialize(run), "party snapshot");
        run.Enter("leader");
        Check(run.RunId != id && run.Phase == RunPhase.Preparing, "fresh run after completion");
        var preparedRun = new DungeonRun();
        var prepare = new PrepareDungeon(id, "party-" + Guid.NewGuid().ToString("N"), ["leader", "member"]);
        Check(PartyRun.ValidId(prepare.PartyId, "party-"), "party ID");
        Check(preparedRun.Prepare(prepare) is null && preparedRun.Prepare(prepare) is null, "idempotent preparation");
        var preparedState = JsonSerializer.Serialize(preparedRun);
        Check(preparedRun.Prepare(prepare with { RunId = "other" })?.Reason == "run-mismatch"
            && preparedRun.Prepare(prepare with { Members = ["other"] })?.Reason == "run-mismatch"
            && JsonSerializer.Serialize(preparedRun) == preparedState, "preparation mismatch immutable");
        preparedRun.Arrived("leader");
        Check(preparedRun.Phase == RunPhase.Entering && preparedRun.Advance()?.Reason == "run-not-active", "partial arrival");
        preparedRun.Arrived("member");
        for (var stage = 1; stage <= GameNames.FinalStage; stage++)
            Check(preparedRun.Advance() is null && preparedRun.Stage == stage, "stage " + stage);
        Check(preparedRun.Phase == RunPhase.Completed && preparedRun.Advance()?.Reason == "run-not-active", "final stage");
        Check(JsonSerializer.Serialize(JsonSerializer.Deserialize<DungeonRun>(JsonSerializer.Serialize(preparedRun)))
            == JsonSerializer.Serialize(preparedRun), "dungeon snapshot");
        Check(JsonSerializer.Serialize(run.View(prepare.PartyId)).Contains("\"Phase\":\"Preparing\""), "enum name JSON");
        Check(JsonSerializer.Serialize(new PartyRun().View(prepare.PartyId)).Contains("\"RunId\":null"), "nullable JSON");
        Console.WriteLine($"Domain checks: {count}/{count} passed");
    }
}
