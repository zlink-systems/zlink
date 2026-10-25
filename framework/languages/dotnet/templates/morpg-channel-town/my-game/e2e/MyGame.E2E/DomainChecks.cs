using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class DomainChecks
{
    public static void Run()
    {
        var count = 0;
        void Check(bool value, string name)
        {
            if (!value) throw new InvalidDataException("Domain: " + name);
            count++;
        }
        Check(ChannelRules.Validate(GameNames.MaximumInteger + 1) == "integer-out-of-range", "channel above maximum");
        Check(ChannelRules.Validate(GameNames.MaximumInteger) == "invalid-channel", "channel maximum");
        Check(ChannelRules.Validate(-GameNames.MaximumInteger - 1) == "integer-out-of-range", "channel below minimum");
        Check(ChannelRules.Validate(0) == "invalid-channel", "channel zero");
        Check(ChannelRules.Validate(1) is null && ChannelRules.Validate(2) is null, "valid channels");
        Check(ChannelRules.Validate(3) == "invalid-channel", "channel above count");
        foreach (var phase in Enum.GetValues<RunPhase>())
        {
            var runId = "run-" + new string('a', 32);
            foreach (var action in new[] { "enter", "prepared", "started", "completed" })
            {
                var sample = new PartyRun { Members = ["leader", "member"], Phase = phase,
                    RunId = phase == RunPhase.Forming ? null : runId };
                var snapshot = JsonSerializer.Serialize(sample);
                var result = action switch
                {
                    "enter" => sample.Enter("leader"),
                    "prepared" => sample.Prepared(runId),
                    "started" => sample.Started(runId),
                    _ => sample.Completed(runId)
                };
                var allowed = action switch
                {
                    "enter" => true,
                    "prepared" => phase is RunPhase.Preparing or RunPhase.Entering,
                    "started" => phase is RunPhase.Entering or RunPhase.Active,
                    _ => phase is RunPhase.Active or RunPhase.Completed
                };
                Check(allowed ? result is null : result == "run-mismatch" && JsonSerializer.Serialize(sample) == snapshot,
                    $"transition table {phase}/{action}");
            }
            var original = new PartyRun { Members = ["leader", "member"], Phase = phase,
                RunId = phase == RunPhase.Forming ? null : runId };
            var encoded = JsonSerializer.Serialize(original);
            Check(JsonSerializer.Serialize(JsonSerializer.Deserialize<PartyRun>(encoded)) == encoded, $"snapshot {phase}");
        }
        var party = new PartyRun { Members = ["leader", "member"] };
        Check(DungeonState.ValidId(party.PartyId, "party-"), "party ID");
        Check(party.Phase == RunPhase.Forming && party.RunId is null, "forming");
        Check(party.ChangeChannel() is null, "forming channel");
        var before = JsonSerializer.Serialize(party);
        Check(party.Enter("member") == "not-leader" && before == JsonSerializer.Serialize(party), "nonleader unchanged");
        Check(party.Started("unknown") == "run-mismatch", "forming start rejected");
        Check(party.Completed("unknown") == "run-mismatch", "forming completion rejected");
        Check(party.Prepared("unknown") == "run-mismatch", "forming preparation rejected");
        Check(party.Enter("leader") is null && party.Phase == RunPhase.Preparing, "prepare transition");
        var run = party.RunId!;
        Check(DungeonState.ValidId(run, "run-"), "run ID");
        Check(party.Enter("leader") is null && party.RunId == run, "preparing retry");
        Check(party.ChangeChannel() == "run-in-progress", "preparing channel");
        party.Leave("member");
        Check(party.Members.Contains("member"), "preparing retains member");
        Check(party.Started(run) == "run-mismatch", "preparing start rejected");
        Check(party.Completed(run) == "run-mismatch", "preparing completion rejected");
        Check(party.Prepared("other") == "run-mismatch", "wrong preparation");
        Check(party.Prepared(run) is null && party.Phase == RunPhase.Entering, "entering transition");
        Check(party.Prepared(run) is null, "prepared retry");
        Check(party.Enter("leader") is null && party.RunId == run, "entering retry");
        Check(party.Completed(run) == "run-mismatch", "entering completion rejected");
        Check(party.ChangeChannel() == "run-in-progress", "entering channel");
        party.Leave("member");
        Check(party.Members.Count == 2, "entering retains member");
        Check(party.Started(run) is null && party.Phase == RunPhase.Active, "active transition");
        Check(party.Started(run) is null, "started retry");
        Check(party.Enter("leader") is null && party.RunId == run, "active enter unchanged");
        Check(party.Prepared(run) == "run-mismatch", "active prepare rejected");
        Check(party.Completed("other") == "run-mismatch", "wrong completion");
        Check(party.ChangeChannel() == "run-in-progress", "active channel");
        party.Leave("member");
        Check(party.Members.Count == 2, "active retains member");
        Check(party.Completed(run) is null && party.Phase == RunPhase.Completed, "completed transition");
        Check(party.Completed(run) is null, "completed retry");
        Check(party.Prepared(run) == "run-mismatch" && party.Started(run) == "run-mismatch", "completed rejects old transitions");
        Check(party.ChangeChannel() == "run-in-progress", "completed channel");
        party.Leave("member");
        Check(party.Members.Count == 2, "completed retains member");
        Check(JsonSerializer.Serialize(JsonSerializer.Deserialize<PartyRun>(JsonSerializer.Serialize(party))) == JsonSerializer.Serialize(party), "party snapshot");
        Check(party.Enter("leader") is null && party.RunId != run && party.Phase == RunPhase.Preparing, "new completed run");
        var forming = new PartyRun { Members = ["leader"] };
        forming.Leave("leader");
        Check(forming.Members.Count == 0, "forming leave");
        var dungeon = new DungeonState();
        var id = GameNames.DungeonId(run);
        var request = new PrepareDungeon(run, party.PartyId, "town-2", ["leader", "member"]);
        Check(dungeon.Advance() == "run-not-active" && dungeon.Stage == 0, "unprepared advance");
        Check(dungeon.Prepare(id, request) is null, "prepare dungeon");
        before = JsonSerializer.Serialize(dungeon);
        Check(dungeon.Prepare(id, request) is null && before == JsonSerializer.Serialize(dungeon), "idempotent prepare");
        Check(dungeon.Prepare(id, request with { RunId = party.RunId! }) == "run-mismatch" && before == JsonSerializer.Serialize(dungeon), "wrong run unchanged");
        Check(dungeon.Prepare(id, request with { Members = ["member", "leader"] }) == "run-mismatch" && before == JsonSerializer.Serialize(dungeon), "wrong members unchanged");
        Check(dungeon.Prepare(id, request with { OwnerSpotId = "town-1" }) == "run-mismatch" && before == JsonSerializer.Serialize(dungeon), "wrong owner unchanged");
        Check(dungeon.Advance() == "run-not-active" && before == JsonSerializer.Serialize(dungeon), "entering advance unchanged");
        Check(!dungeon.Accepts("other", id, new(id, run)), "nonmember admission");
        Check(!dungeon.Accepts("leader", id, new(id, "other")), "wrong run admission");
        Check(dungeon.Accepts("leader", id, new(id, run)), "member admission");
        Check(!dungeon.Join("other") && dungeon.Joined.Count == 0, "unknown join unchanged");
        Check(!dungeon.Join("leader") && dungeon.Phase == RunPhase.Entering, "partial arrival");
        Check(!dungeon.Join("leader") && dungeon.Joined.Count == 1, "duplicate arrival");
        Check(dungeon.Join("member") && dungeon.Phase == RunPhase.Active, "all arrive");
        Check(dungeon.Advance() is null && dungeon.Stage == 1, "advance one");
        Check(dungeon.Advance() is null && dungeon.Stage == 2, "advance two");
        Check(dungeon.Advance() is null && dungeon.Stage == GameNames.FinalStage && dungeon.Phase == RunPhase.Completed, "final stage");
        before = JsonSerializer.Serialize(dungeon);
        Check(dungeon.Advance() == "run-not-active" && before == JsonSerializer.Serialize(dungeon), "completed advance unchanged");
        Check(JsonSerializer.Serialize(JsonSerializer.Deserialize<DungeonState>(before)) == before, "dungeon snapshot");
        dungeon.Stage = GameNames.MaximumInteger;
        before = JsonSerializer.Serialize(dungeon);
        Check(dungeon.Advance() == "integer-out-of-range" && JsonSerializer.Serialize(dungeon) == before, "integer boundary unchanged");
        dungeon.Stage = GameNames.MaximumInteger + 1;
        Check(dungeon.Advance() == "integer-out-of-range", "integer above bound");
        Check(JsonSerializer.Serialize(RunPhase.Active) == "\"Active\"", "enum name");
        Check(JsonSerializer.Serialize(new PartyRun()).Contains("\"RunId\":null"), "nullable run");
        Check(new ActionRejected("run-not-active").RetryAfterMs == 0, "rejection time");
        Console.WriteLine($"Domain checks: {count}/{count} passed");
    }
}
