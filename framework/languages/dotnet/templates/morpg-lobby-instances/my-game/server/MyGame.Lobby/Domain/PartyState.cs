using MyGame.Shared;

namespace MyGame.Lobby.Domain;

public sealed class PartyState
{
    public string PartyId { get; set; } = "";
    public List<string> Members { get; set; } = [];
    public HashSet<string> Ready { get; set; } = new(StringComparer.Ordinal);

    public bool Join(string actorId, PartyJoin request)
    {
        if (
            string.IsNullOrWhiteSpace(request.PartyId)
            || Members.Contains(actorId)
            || Members.Count >= GameNames.MaximumPartyMembers
            || (
                request.Create
                    ? Members.Count != 0
                    : PartyId != request.PartyId || Members.Count == 0
            )
        )
            return false;
        PartyId = request.PartyId;
        Members.Add(actorId);
        return true;
    }

    public void MarkReady(string actorId) => Ready.Add(actorId);

    public void Leave(string actorId)
    {
        Members.Remove(actorId);
        Ready.Remove(actorId);
    }

    public PartyView View() =>
        new(PartyId, Members.ToArray(), Members.Select(Ready.Contains).ToArray());

    public string? EntryRejection(string actorId) =>
        Members.Count == 0 || Members[0] != actorId ? Rejection.NotLeader
        : Members.Any(id => !Ready.Contains(id)) ? Rejection.NotReady
        : null;
}
