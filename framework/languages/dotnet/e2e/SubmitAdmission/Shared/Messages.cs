namespace SubmitAdmission.Shared;

public static class SubmitAdmissionNames
{
    public const string Mesh = "submit-admission.mesh";
    public const string Channel = "submit-admission.channel";
    public const string Fanout = "submit-admission.fanout";
}

public sealed record AdmissionMsg(string OperationId, int Sequence, string Payload);

public sealed record AdmissionEvent(string OperationId, int Sequence, string Payload);

public sealed record RouteReadyReq(string Marker);

public sealed record RouteReadyRes(string Rid, string Marker);

public sealed record SubmitRes(
    string OperationId,
    string Family,
    string Status,
    int PublicInvocationCount,
    int TerminalCount);

public sealed record FillRes(
    string OperationId,
    bool Pending,
    int StartedCount,
    string? TerminalStatus);

public sealed record CancellationRes(
    string OperationId,
    string Outcome,
    string ExceptionType,
    int PublicInvocationCount,
    int InvalidInvocationCount,
    int TerminalCount);

public sealed record NodeTargetOutcomeRes(
    string Send,
    string Request,
    int PeerCountBefore,
    int PeerCountAfter,
    int ReadyPeerCountBefore,
    int ReadyPeerCountAfter);

public sealed record ObjectClientIdentityRes(string Rid, string Endpoint);

public sealed record OperationEvidenceRes(
    string OperationId,
    string Family,
    string TargetId,
    int PublicInvocationCount,
    int InvalidInvocationCount,
    DateTimeOffset? PendingAt,
    int TransportAttemptCount,
    int CommitCount,
    int SendReadySignalCount,
    int RetryAttemptCount,
    int PendingWaiterCount,
    int ReservationCount,
    int CallbackCount,
    int TerminalCount,
    string? TerminalStatus,
    DateTimeOffset? TerminalAt,
    int HandlerEnteredCount,
    DateTimeOffset? HandlerEnteredAt,
    int HandlerCompletedCount,
    DateTimeOffset? HandlerCompletedAt);
