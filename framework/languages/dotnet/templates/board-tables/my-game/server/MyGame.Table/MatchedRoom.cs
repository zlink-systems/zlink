using MyGame.Shared;

namespace MyGame.Table;

public enum RoomPhase
{
    Prepared,
    Committed,
    Withdrawn,
}

public sealed record MatchedRoom(
    string AssignmentId,
    string[] Members,
    RoomPhase Phase = RoomPhase.Prepared
)
{
    public bool Admits(string roomId, string playerId, MatchAssigned? admission) =>
        Phase == RoomPhase.Prepared
        && admission is not null
        && admission.AssignmentId == AssignmentId
        && admission.RoomId == roomId
        && Members.Contains(playerId, StringComparer.Ordinal);
}
