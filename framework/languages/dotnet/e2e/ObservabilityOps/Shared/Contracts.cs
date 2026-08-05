namespace ObservabilityOps.Shared;

public static class ObservabilityNames
{
    public const string PlayMesh = "observability.play";
    public const string PlayerActorType = "observability-player";
    public const string StreamNode = "observability-session";
    public const string WorkflowMesh = "observability.workflow";
}

public sealed record AuthenticateReq(string ActorId);
public sealed record AuthenticateRes(string ActorId, string NodeRid, ulong Generation);
public sealed record SessionBoundedOperationReq(string Marker);
public sealed record SessionBoundedOperationRes(string Marker);
public sealed record EnsurePlayerReq(string ActorId);
public sealed record EnsurePlayerRes(string ActorId, string NodeRid, ulong Generation);
public sealed record PlayBoundedOperationReq(string RoomId, string Marker);
public sealed record PlayBoundedOperationRes(string Marker, string NodeRid);
public sealed record ActivateInstanceSpotReq(string Marker);
public sealed record ActivateInstanceSpotRes(
    string SpotId,
    string NodeRid,
    string Marker);
// Runner-only control changes the local placement weight so a scenario can
// establish the node-specific precondition stated by its E2E contract.
public sealed record PlacementWeightReq(int Weight);
public sealed record PlacementWeightRes(int Weight);
public sealed record CreateRoomReq(string RoomRid, string Mode = "normal");
public sealed record CreateRoomRes(string RoomRid, string NodeRid);
public sealed record JoinRoomReq(string RoomRid);
public sealed record JoinRoomRes(string ActorId, string RoomRid, string NodeRid);
public sealed record ActorJoinCompletedNotify(
    string ActorId,
    string? SpotId,
    string NodeRid,
    string Marker,
    bool Accepted,
    string? Error);
public sealed record GameActionReq(string Marker, int WorkMilliseconds = 0);
public sealed record GameActionRes(string ActorId, string RoomRid, string NodeRid, string Marker);
public sealed record PlayerMovedNotify(string ActorId, string TargetNodeRid);
public sealed record ReturnToLobbyReq(string Marker);
public sealed record ReturnToLobbyRes(string ActorId, string NodeRid, string Marker);
public sealed record CreateWorkflowReq(string WorkflowRid, string Kind = "owner");
public sealed record CreateWorkflowRes(string WorkflowRid, string NodeRid, int Version, string State);
public sealed record AdvanceWorkflowReq(string Marker);
public sealed record AdvanceWorkflowRes(string WorkflowRid, string NodeRid, int Version, string State);
public sealed record ReadWorkflowReq;
public sealed record ReadWorkflowRes(string WorkflowRid, string NodeRid, int Version, string State);
public sealed record WorkflowSignalReq(string Marker);
public sealed record DiagnosticsBeforeReq(string Marker);
public sealed record DiagnosticsOffReq(string Marker);
public sealed record DiagnosticsErrorsReq(string Marker);
public sealed record DiagnosticsAfterReq(string Marker);
public sealed record DiagnosticsProbeRes(string Marker, string NodeRid);
public sealed record StaleSpotIdProbeRes(bool Failed, string? ErrorKind);
public sealed record PublishProjectionReq(string Marker);
public sealed record PublishProjectionRes(string WorkflowRid, int Version);
public sealed record ProjectionUpdatedEvent(string WorkflowRid, int Version, string Marker);
public sealed record EvidenceWaitReq(
    string[] ContainsAll,
    string[][] ContainsAnyGroups,
    int TimeoutMilliseconds = 10000);
public sealed record MetricWaitReq(
    string Name,
    decimal MinimumValue,
    decimal? MaximumValue = null,
    IReadOnlyDictionary<string, string>? RequiredTags = null,
    int TimeoutMilliseconds = 10000);
public sealed record EvidenceSnapshot(
    string Role,
    bool Ready,
    string[] Entries,
    MetricSample[] Metrics,
    PeerRow[] PeerRows,
    ActorRow[] ActorRows,
    SpotRow[] SpotRows);
public sealed record MetricSample(
    string Name, string Kind, decimal Value, string? Unit,
    IReadOnlyDictionary<string, string> Tags, long Count,
    decimal? Sum, decimal? Min, decimal? Max);
public sealed record PeerRow(string NodeRid, bool Draining, long Generation);
public sealed record ActorRow(string ActorId, string NodeRid, long Generation);
public sealed record SpotRow(string MeshName, string NodeRid, string SpotRid, string Kind, long Generation);
public sealed record MaintenanceStatus(
    bool Started,
    bool Completed,
    string? Result,
    string? Reason,
    string? Error,
    int TerminalCount = 0);
public sealed record RelocateHostReq(
    string Mode,
    long? TargetApplicationVersion,
    int DeadlineMilliseconds = 30000);
public sealed record RelocateHostRes(
    string Mode,
    long TargetApplicationVersion,
    string Outcome,
    string Reason);
public sealed record ShutdownHostReq(int DeadlineMilliseconds = 30000);
public sealed record ShutdownHostRes(string Outcome, string Reason);
public sealed record RuntimeStatusRes(
    string State,
    bool IsReady,
    bool AcceptingWork,
    ulong Sequence);
public sealed record NodeIdentityRes(string Role, string NodeRid);
