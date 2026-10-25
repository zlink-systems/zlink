using MyGame.Shared;

namespace MyGame.Lobby.Domain;

public sealed class PartyState
{
    public string PartyId { get; set; } = "";
    public List<string> Members { get; set; } = [];
    public HashSet<string> Ready { get; set; } = new(StringComparer.Ordinal);
    public HashSet<string> Entered { get; set; } = new(StringComparer.Ordinal);
    public RunPhase Phase { get; set; } = RunPhase.Forming;
    public string? RunId { get; set; }

    public bool Join(string actorId, PartyJoin request)
    {
        if (Phase != RunPhase.Forming || string.IsNullOrWhiteSpace(request.PartyId)
            || Members.Contains(actorId) || Members.Count >= GameNames.MaximumPartyMembers
            || (request.Create ? Members.Count != 0 : PartyId != request.PartyId || Members.Count == 0))
            return false;
        PartyId = request.PartyId;
        Members.Add(actorId);
        return true;
    }

    public void MarkReady(string actorId)
    {
        if (Phase == RunPhase.Forming && Members.Contains(actorId)) Ready.Add(actorId);
    }

    public void Leave(string actorId)
    {
        if (Phase != RunPhase.Forming) return;
        Members.Remove(actorId);
        Ready.Remove(actorId);
    }

    public PartyView View() => new(PartyId, Members.ToArray(), Members.Select(Ready.Contains).ToArray(), Phase, RunId);

    public string? Enter(string actorId)
    {
        if (Members.Count == 0 || Members[0] != actorId) return Rejection.NotLeader;
        if (Members.Any(id => !Ready.Contains(id))) return Rejection.NotReady;
        if (Phase is RunPhase.Forming or RunPhase.Completed)
        {
            RunId = GameNames.NewRunId();
            Phase = RunPhase.Preparing;
            Entered.Clear();
        }
        return null;
    }

    public string? Arrived(string runId, string actorId)
    {
        if (RunId != runId || Phase == RunPhase.Forming || !Members.Contains(actorId)) return Rejection.RunMismatch;
        Entered.Add(actorId); return null;
    }

    public string? Prepared(string runId)
    {
        if (RunId != runId || Phase is not (RunPhase.Preparing or RunPhase.Entering)) return Rejection.RunMismatch;
        Phase = RunPhase.Entering;
        return null;
    }

    public string? Started(string runId)
    {
        if (RunId != runId || Phase is not (RunPhase.Entering or RunPhase.Active)) return Rejection.RunMismatch;
        Phase = RunPhase.Active;
        return null;
    }

    public string? Completed(string runId)
    {
        if (RunId != runId || Phase is not (RunPhase.Active or RunPhase.Completed)) return Rejection.RunMismatch;
        Phase = RunPhase.Completed;
        return null;
    }
}
