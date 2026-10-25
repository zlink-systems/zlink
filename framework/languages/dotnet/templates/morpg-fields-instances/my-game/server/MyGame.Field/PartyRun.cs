using MyGame.Shared;

namespace MyGame.Field;

public sealed class PartyRun
{
    public List<string> Members { get; set; } = [];
    public HashSet<string> Entered { get; set; } = new(StringComparer.Ordinal);
    public RunPhase Phase { get; set; }
    public string? RunId { get; set; }

    public static bool ValidId(string? id, string prefix) => id is not null
        && id.StartsWith(prefix, StringComparison.Ordinal) && id.Length == prefix.Length + 32
        && id.AsSpan(prefix.Length).IndexOfAnyExcept("0123456789abcdef") < 0;

    public ActionRejected? Join(string id)
    {
        if (Phase != RunPhase.Forming || Members.Contains(id)
            || Members.Count >= GameNames.MaximumPartyMembers || string.IsNullOrWhiteSpace(id))
            return new("invalid-party");
        Members.Add(id);
        return null;
    }

    public void Leave(string id)
    {
        if (Phase == RunPhase.Forming) Members.Remove(id);
    }

    public ActionRejected? Enter(string id)
    {
        if (Members.Count == 0 || Members[0] != id) return new("not-leader");
        if (Phase is RunPhase.Forming or RunPhase.Completed)
        {
            RunId = "run-" + Guid.NewGuid().ToString("N");
            Entered.Clear();
            Phase = RunPhase.Preparing;
        }
        return null;
    }

    public ActionRejected? Prepared(string runId)
    {
        if (runId != RunId || Phase is not (RunPhase.Preparing or RunPhase.Entering))
            return new("run-mismatch");
        Phase = RunPhase.Entering;
        return null;
    }

    public ActionRejected? Arrived(string runId, string actorId)
    {
        if (runId != RunId || !Members.Contains(actorId)
            || Phase is not (RunPhase.Entering or RunPhase.Active)) return new("run-mismatch");
        Entered.Add(actorId);
        return null;
    }

    public ActionRejected? Started(string runId)
    {
        if (runId != RunId || Phase is not (RunPhase.Entering or RunPhase.Active)
            || Entered.Count != Members.Count) return new("run-mismatch");
        Phase = RunPhase.Active;
        return null;
    }

    public ActionRejected? Completed(string runId)
    {
        if (runId != RunId || Phase is not (RunPhase.Active or RunPhase.Completed))
            return new("run-mismatch");
        Phase = RunPhase.Completed;
        return null;
    }

    public PartyView View(string partyId) => new(partyId, Members.ToArray(),
        Members.FirstOrDefault() ?? "", Phase, RunId);
}
