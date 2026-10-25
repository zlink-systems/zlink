using MyGame.Shared;

namespace MyGame.Instance;

public sealed class DungeonRun
{
    public string PartyId { get; set; } = "";
    public string RunId { get; set; } = "";
    public string[] Members { get; set; } = [];
    public HashSet<string> Entered { get; set; } = new(StringComparer.Ordinal);
    public RunPhase Phase { get; set; } = RunPhase.Preparing;
    public int Stage { get; set; }

    public ActionRejected? Prepare(PrepareDungeon request)
    {
        if (RunId.Length != 0)
            return RunId == request.RunId && PartyId == request.PartyId
                && Members.SequenceEqual(request.Members) ? null : new("run-mismatch");
        if (request.Members.Length == 0 || request.Members.Length > GameNames.MaximumPartyMembers
            || request.Members.Distinct().Count() != request.Members.Length
            || request.Members.Any(string.IsNullOrWhiteSpace)) return new("invalid-party");
        RunId = request.RunId;
        PartyId = request.PartyId;
        Members = request.Members.ToArray();
        Phase = RunPhase.Entering;
        return null;
    }
    public bool CanJoin(string actorId, string runId) => RunId == runId && Members.Contains(actorId);
    public void Arrived(string actorId)
    {
        Entered.Add(actorId);
        if (Entered.Count == Members.Length && Phase == RunPhase.Entering) Phase = RunPhase.Active;
    }
    public ActionRejected? Advance()
    {
        if (Phase != RunPhase.Active) return new("run-not-active");
        Stage++;
        if (Stage == GameNames.FinalStage) Phase = RunPhase.Completed;
        return null;
    }
}
