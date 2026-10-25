using System.Text.Json;
using System.Text.RegularExpressions;
using MyGame.Lobby.Domain;
using MyGame.Instance;
using MyGame.Shared;
namespace MyGame.E2E;
internal static class DomainChecks
{
    public static void Run()
    {
        var count = 0;
        void Check(bool value, string name) { if (!value) throw new InvalidDataException("Domain check failed: " + name); count++; }
        string Snap<T>(T value) => JsonSerializer.Serialize(value);
        PartyState Party(RunPhase phase = RunPhase.Forming) => new() { PartyId = GameNames.NewPartyId(), Members = ["leader", "member"], Ready = ["leader", "member"], Phase = phase, RunId = phase == RunPhase.Forming ? null : GameNames.NewRunId() };
        Check(Regex.IsMatch(GameNames.NewPartyId(), "^party-[0-9a-f]{32}$"), "party ID");
        Check(Regex.IsMatch(GameNames.NewRunId(), "^run-[0-9a-f]{32}$"), "run ID");
        Check(GameNames.MaximumInteger == 9_007_199_254_740_991 && GameNames.FinalStage == 3, "bounds");
        foreach (var phase in Enum.GetValues<RunPhase>())
        {
            var p = Party(phase); var before = Snap(p);
            Check(p.Enter("member") == Rejection.NotLeader && Snap(p) == before, phase + " non-leader unchanged");
            p.Ready.Remove("member"); before = Snap(p);
            Check(p.Enter("leader") == Rejection.NotReady && Snap(p) == before, phase + " not-ready unchanged");
            p.Ready.Add("member"); var old = p.RunId;
            Check(p.Enter("leader") is null, phase + " leader");
            Check(phase is RunPhase.Forming or RunPhase.Completed ? p.Phase == RunPhase.Preparing && p.RunId != old : p.Phase == phase && p.RunId == old, phase + " entry");
            old = p.RunId; Check(p.Enter("leader") is null && p.RunId == old, phase + " repeated entry");
            p = Party(phase); before = Snap(p);
            Check(p.Prepared("wrong") == Rejection.RunMismatch && Snap(p) == before, phase + " prepared mismatch");
            Check((p.Prepared(p.RunId ?? "") is null) == (phase is RunPhase.Preparing or RunPhase.Entering), phase + " prepared table");
            p = Party(phase); before = Snap(p);
            Check(p.Started("wrong") == Rejection.RunMismatch && Snap(p) == before, phase + " started mismatch");
            Check((p.Started(p.RunId ?? "") is null) == (phase is RunPhase.Entering or RunPhase.Active), phase + " started table");
            p = Party(phase); before = Snap(p);
            Check(p.Completed("wrong") == Rejection.RunMismatch && Snap(p) == before, phase + " completed mismatch");
            Check((p.Completed(p.RunId ?? "") is null) == (phase is RunPhase.Active or RunPhase.Completed), phase + " completed table");
            p = Party(phase); p.Leave("member");
            Check(p.Members.Contains("member") == (phase != RunPhase.Forming), phase + " leave");
            Check(Snap(JsonSerializer.Deserialize<PartyState>(Snap(p))) == Snap(p), phase + " party snapshot");
            var dungeon = new DungeonState { Phase = phase }; before = Snap(dungeon);
            Check((dungeon.Advance() is null) == (phase == RunPhase.Active), phase + " advance table");
            Check(phase == RunPhase.Active || Snap(dungeon) == before, phase + " rejected advance unchanged");
        }
        var d = new DungeonState(); var request = new PrepareDungeon(GameNames.NewRunId(), GameNames.NewPartyId(), ["leader", "member"]);
        Check(d.Prepare(request) is null, "prepare"); var snapshot = Snap(d);
        Check(d.Prepare(request) is null && Snap(d) == snapshot, "prepare idempotence");
        Check(d.Prepare(request with { RunId = GameNames.NewRunId() }) == Rejection.RunMismatch && Snap(d) == snapshot, "run mismatch");
        Check(d.Prepare(request with { Members = ["leader"] }) == Rejection.RunMismatch && Snap(d) == snapshot, "roster mismatch");
        Check(d.Prepare(request with { PartyId = GameNames.NewPartyId() }) == Rejection.RunMismatch && Snap(d) == snapshot, "party mismatch");
        Check(!d.Admit("outsider", new(GameNames.DungeonId(request.RunId), request.RunId), GameNames.DungeonId(request.RunId)), "admit roster");
        Check(!d.Join("outsider") && Snap(d) == snapshot, "outsider unchanged");
        Check(!d.Join("leader") && d.Phase == RunPhase.Entering, "partial arrival");
        Check(!d.Join("leader") && d.Phase == RunPhase.Entering, "duplicate arrival");
        Check(d.Join("member") && d.Phase == RunPhase.Active, "all arrived");
        Check(!d.Join("member") && d.Phase == RunPhase.Active, "started idempotence");
        for (var stage = 1; stage <= GameNames.FinalStage; stage++) Check(d.Advance() is null && d.Stage == stage && d.Phase == (stage == GameNames.FinalStage ? RunPhase.Completed : RunPhase.Active), "stage " + stage);
        snapshot = Snap(d); Check(d.Advance() == Rejection.RunNotActive && Snap(d) == snapshot, "completed unchanged");
        Check(Snap(JsonSerializer.Deserialize<DungeonState>(snapshot)) == snapshot, "dungeon snapshot");
        foreach (var value in new[] { -1L, GameNames.MaximumInteger, GameNames.MaximumInteger + 1, long.MaxValue })
        {
            d = new() { Stage = value, Phase = RunPhase.Active }; snapshot = Snap(d);
            Check(d.Advance() == Rejection.IntegerOutOfRange && Snap(d) == snapshot, "integer boundary " + value);
        }
        var arrivals = Party(RunPhase.Entering); var arrivalSnapshot = Snap(arrivals);
        Check(arrivals.Arrived("wrong", "member") == Rejection.RunMismatch && Snap(arrivals) == arrivalSnapshot, "arrival run mismatch");
        Check(arrivals.Arrived(arrivals.RunId!, "outsider") == Rejection.RunMismatch && Snap(arrivals) == arrivalSnapshot, "arrival roster mismatch");
        Check(arrivals.Arrived(arrivals.RunId!, "member") is null && arrivals.Entered.Contains("member"), "arrival recorded");
        arrivalSnapshot = Snap(arrivals);
        Check(arrivals.Arrived(arrivals.RunId!, "member") is null && Snap(arrivals) == arrivalSnapshot, "arrival idempotent");
        Check(Snap(JsonSerializer.Deserialize<PartyState>(arrivalSnapshot)) == arrivalSnapshot, "arrival snapshot");
        arrivals.Phase = RunPhase.Completed;
        Check(arrivals.Enter("leader") is null && arrivals.Entered.Count == 0, "new run clears arrivals");
        Check(Snap(Party().View()).Contains("\"RunId\":null") && Snap(Party().View()).Contains("\"Phase\":\"Forming\""), "enum null JSON");
        Console.WriteLine($"Domain checks: {count}/{count} passed");
    }
}
