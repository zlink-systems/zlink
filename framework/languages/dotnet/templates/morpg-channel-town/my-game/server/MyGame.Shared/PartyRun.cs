namespace MyGame.Shared;

public sealed class PartyRun
{
    public string PartyId { get; set; } = "party-" + Guid.NewGuid().ToString("N");
    public List<string> Members { get; set; } = [];
    public RunPhase Phase { get; set; }
    public string? RunId { get; set; }

    public string? Enter(string player)
    {
        if (Members.Count == 0 || Members[0] != player) return Rejection.NotLeader;
        if (Phase is RunPhase.Forming or RunPhase.Completed)
        {
            RunId = "run-" + Guid.NewGuid().ToString("N");
            Phase = RunPhase.Preparing;
        }
        return null;
    }

    public string? Prepared(string runId)
    {
        if (RunId != runId || Phase is not (RunPhase.Preparing or RunPhase.Entering))
            return Rejection.RunMismatch;
        Phase = RunPhase.Entering;
        return null;
    }

    public string? Started(string runId)
    {
        if (RunId != runId || Phase is not (RunPhase.Entering or RunPhase.Active))
            return Rejection.RunMismatch;
        Phase = RunPhase.Active;
        return null;
    }

    public string? Completed(string runId)
    {
        if (RunId != runId || Phase is not (RunPhase.Active or RunPhase.Completed))
            return Rejection.RunMismatch;
        Phase = RunPhase.Completed;
        return null;
    }

    public void Leave(string player)
    {
        if (Phase == RunPhase.Forming) Members.Remove(player);
    }

    public string? ChangeChannel() => Phase == RunPhase.Forming ? null : Rejection.RunInProgress;
    public PartyView View() => new(PartyId, Members.ToArray(), Phase, RunId);
}

public sealed class DungeonState
{
    public string PartyId { get; set; } = "";
    public string RunId { get; set; } = "";
    public string OwnerSpotId { get; set; } = "";
    public string[] Members { get; set; } = [];
    public HashSet<string> Joined { get; set; } = [];
    public RunPhase Phase { get; set; }
    public long Stage { get; set; }

    public string? Prepare(string dungeonId, PrepareDungeon request)
    {
        if (RunId.Length != 0)
            return RunId == request.RunId && PartyId == request.PartyId
                && OwnerSpotId == request.OwnerSpotId
                && Members.SequenceEqual(request.Members) ? null : Rejection.RunMismatch;
        if (!ValidId(request.RunId, "run-") || !ValidId(request.PartyId, "party-")
            || string.IsNullOrWhiteSpace(request.OwnerSpotId)
            || GameNames.DungeonId(request.RunId) != dungeonId
            || request.Members.Length is < 1 or > GameNames.MaximumPartyMembers
            || request.Members.Distinct().Count() != request.Members.Length
            || request.Members.Any(string.IsNullOrWhiteSpace)) return Rejection.RunMismatch;
        RunId = request.RunId;
        PartyId = request.PartyId;
        OwnerSpotId = request.OwnerSpotId;
        Members = request.Members.ToArray();
        Phase = RunPhase.Entering;
        return null;
    }

    public static bool ValidId(string value, string prefix) => value.StartsWith(prefix, StringComparison.Ordinal)
        && value.Length == prefix.Length + 32
        && value.AsSpan(prefix.Length).IndexOfAnyExcept("0123456789abcdef") < 0;

    public bool Accepts(string actor, string dungeonId, JoinDungeon request) =>
        request.DungeonId == dungeonId && request.RunId == RunId && Members.Contains(actor);

    public bool Join(string actor)
    {
        if (!Members.Contains(actor)) return false;
        Joined.Add(actor);
        if (Phase != RunPhase.Entering || Joined.Count != Members.Length) return false;
        Phase = RunPhase.Active;
        return true;
    }

    public string? Advance()
    {
        if (Stage < 0 || Stage >= GameNames.MaximumInteger) return Rejection.IntegerOutOfRange;
        if (Phase != RunPhase.Active) return Rejection.RunNotActive;
        Stage++;
        if (Stage == GameNames.FinalStage) Phase = RunPhase.Completed;
        return null;
    }
}

public static class ChannelRules
{
    public static string? Validate(long channel) =>
        channel > GameNames.MaximumInteger || channel < -GameNames.MaximumInteger
            ? Rejection.IntegerOutOfRange
            : channel < 1 || channel > GameNames.ChannelCount ? Rejection.InvalidChannel : null;
}
