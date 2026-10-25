using MyGame.Shared;

namespace MyGame.Instance;

public sealed class DungeonState
{
    public string PartyId { get; set; } = "";
    public string? RunId { get; set; }
    public string[] Members { get; set; } = [];
    public HashSet<string> Joined { get; set; } = new(StringComparer.Ordinal);
    public long Stage { get; set; }
    public RunPhase Phase { get; set; } = RunPhase.Preparing;

    public string? Prepare(PrepareDungeon request)
    {
        if (RunId is not null)
            return RunId == request.RunId && PartyId == request.PartyId && Members.SequenceEqual(request.Members)
                ? null : Rejection.RunMismatch;
        if (string.IsNullOrWhiteSpace(request.RunId) || string.IsNullOrWhiteSpace(request.PartyId)
            || request.Members.Length == 0 || request.Members.Length > GameNames.MaximumPartyMembers
            || request.Members.Distinct().Count() != request.Members.Length || request.Members.Any(string.IsNullOrWhiteSpace))
            return Rejection.RunMismatch;
        RunId = request.RunId;
        PartyId = request.PartyId;
        Members = request.Members.ToArray();
        Phase = RunPhase.Entering;
        return null;
    }

    public bool Admit(string actorId, JoinDungeon request, string dungeonId) =>
        request.DungeonId == dungeonId && request.RunId == RunId && Members.Contains(actorId);

    public bool Join(string actorId)
    {
        if (!Members.Contains(actorId)) return false;
        Joined.Add(actorId);
        if (Phase != RunPhase.Entering || Joined.Count != Members.Length) return false;
        Phase = RunPhase.Active;
        return true;
    }

    public string? Advance()
    {
        if (Stage >= GameNames.MaximumInteger || Stage < 0) return Rejection.IntegerOutOfRange;
        if (Phase != RunPhase.Active) return Rejection.RunNotActive;
        Stage++;
        if (Stage == GameNames.FinalStage) Phase = RunPhase.Completed;
        return null;
    }
}
