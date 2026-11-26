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
    public bool Admits(string playerId, RoomAdmission? admission) =>
        Phase == RoomPhase.Prepared
        && admission is not null
        && admission.AssignmentId == AssignmentId
        && Members.Contains(playerId, StringComparer.Ordinal);
}
