namespace MyGame.Shared;

public static class MatchNames
{
    public const string PoolType = "Pool";
    public const string PoolKey = "pool-board";
    public const string RoomProvisionerType = "RoomProvisioner";
    public const string RoomProvisionerKey = "room-provisioner-board";
    public const string Cancelled = "cancelled";
    public const string Expired = "expired";
    public const string Disconnected = "disconnected";
}

public sealed record FindMatch;

public sealed record MatchQueued(string TicketId);

public sealed record CancelMatch(string TicketId);

public sealed record MatchCancelled(string TicketId, string Reason);

public sealed record MatchFound(string TicketId, string RoomId, int Seat);

public sealed record MatchRequest(string RequestKey, string PlayerId);

public sealed record PrepareRoom(string AssignmentId, string[] Members);

public sealed record RoomPrepared(string AssignmentId, bool Ok);

public sealed record MatchAssigned(string TicketId, string AssignmentId, string RoomId);

public sealed record EnterReport(string TicketId, string AssignmentId, bool Ok);

public sealed record CommitRoom(string AssignmentId);

public sealed record WithdrawRoom(string AssignmentId);

public sealed record CancelTicket(string TicketId, string Reason);

public sealed record RoomAdmission(string AssignmentId, long Chips);

public sealed record TableAdmission(JoinTableRequest Join, long Chips);
