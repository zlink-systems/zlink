using System.Text.Json;
using MyGame.Shared;

namespace MyGame.Match.Domain;

public sealed class RoomRoster
{
    private string? matchId;
    private HashSet<string> members = new(StringComparer.Ordinal);

    public RoomPrepared Prepare(string roomId, PrepareRoom request)
    {
        if (
            string.IsNullOrWhiteSpace(roomId)
            || request is null
            || request.MatchId != roomId
            || request.Members is null
            || request.Members.Length != GameNames.PlayersPerRoom
            || request.Members.Any(string.IsNullOrWhiteSpace)
            || request.Members.Distinct(StringComparer.Ordinal).Count() != request.Members.Length
        )
            throw new InvalidDataException("Invalid room roster preparation.");

        if (matchId is not null)
        {
            if (matchId != request.MatchId || !members.SetEquals(request.Members))
                throw new InvalidOperationException("A prepared Room roster cannot be replaced.");
            return new RoomPrepared(matchId);
        }

        matchId = request.MatchId;
        members = new HashSet<string>(request.Members, StringComparer.Ordinal);
        return new RoomPrepared(matchId);
    }

    public bool CanAdmit(string playerId) => matchId is not null && members.Contains(playerId);

    public RosterSnapshot Capture() =>
        new(matchId, members.OrderBy(member => member, StringComparer.Ordinal).ToArray());

    public void Restore(RosterSnapshot snapshot)
    {
        if (matchId is not null)
            throw new InvalidOperationException("Cannot restore an already prepared Room roster.");
        if (snapshot.Members is null)
            throw new InvalidDataException("Missing Room roster members.");
        if (snapshot.MatchId is not null)
            Prepare(snapshot.MatchId, new PrepareRoom(snapshot.MatchId, snapshot.Members));
        else if (snapshot.Members.Length != 0)
            throw new InvalidDataException("Unprepared Room snapshot has members.");
    }
}

public sealed record RosterSnapshot(string? MatchId, string[] Members);

public sealed record HttpRoomSnapshot(JsonElement Game, RosterSnapshot Roster);
