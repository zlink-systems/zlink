namespace AutomaticTurnDispatch.Shared;

public static class AutomaticTurnDispatchNames
{
    public const string ControlChannel = "await.control";
    public const string DelayChannel = "await.delay";
    public const string SpotChannel = "await.spot";
    public const string SpotRouteChannel = "await.spot.route";
    public const string StreamNode = "await.stream";
    public const string ActorType = "await.actor";
    public const string SpotType = "await.spot.user";
    public const string PerActorSpotType = "await.spot.per-actor";
    public const string ActorIdMetadata = "actor-id";
    public const string SpotRidMetadata = "spot-rid";
    public const string TargetNodeRidMetadata = "target-node-rid";
}

public sealed record AwaitShutdownScenarioReq(string RequestId, string SpotRid, int DelayMs);

public sealed record AwaitShutdownRecoveryReq(string RequestId, string SpotRid);

public sealed record BindAwaitActorsReq(
    string SpotRid,
    string[] ActorIds,
    string SpotType = AutomaticTurnDispatchNames.SpotType);

public sealed record BindAwaitActorsRes(string SpotRid, AwaitActorBinding[] Actors);

public sealed record AwaitActorBinding(string ActorId, string NodeRid, ulong Generation);

public sealed record AwaitEvidenceReq(string RequestId);

public sealed record AwaitEvidenceRes(string RequestId, string[] Evidence);

public sealed record AwaitEvidenceWaitReq(
    string RequestId,
    string Marker,
    int TimeoutMilliseconds = 20000,
    int MinimumCount = 1);

public sealed record DelayReq(string RequestId, int DelayMs, string Marker);

public sealed record JoinDelayReq(
    string RequestId,
    int DelayMs,
    string CompletionMarker);

public sealed record DelayRes(string RequestId, string Marker, string NodeRid);

public sealed record ExternalDelayRes(string RequestId, string Marker);

public sealed record HoldReq(string RequestId, int DelayMs);

public sealed record HoldMsg(string RequestId, int DelayMs);

public sealed record AwaitReq(string RequestId, int DelayMs, string CorrelationId, string Terminator = "async");

public sealed record AwaitMsg(string RequestId, int DelayMs, string CorrelationId, string Terminator = "async");

public sealed record CounterResetMsg(string RequestId);

public sealed record CounterAwaitMsg(string RequestId, string OperationId, int DelayMs, string Terminator);

public sealed record CounterReadReq(string RequestId);

public sealed record CounterReadRes(string RequestId, int Value);

public sealed record HttpAwaitMsg(string RequestId, int DelayMs, string Terminator);

public sealed record IoWorkerAwaitMsg(string RequestId, string OperationId, int DelayMs);

public sealed record CpuWorkerAwaitMsg(string RequestId, int DelayMs, string Terminator);

public sealed record SelfCycleMsg(
    string RequestId,
    int TimeoutMs,
    string Terminator = "async");

public sealed record SelfSendMsg(string RequestId, string Marker);

public sealed record DeferredJoinFailureMsg(
    string RequestId,
    string FirstActorId,
    string SecondActorId,
    string FirstTargetSpotRid,
    string SecondTargetSpotRid,
    string FailureMode);

public sealed record WorkerAwaitReq(string RequestId, int DelayMs);

public sealed record WorkerAwaitMsg(string RequestId, int DelayMs);

public sealed record AwaitTimeoutReq(string RequestId, int DelayMs, int TimeoutMs);

public sealed record AwaitTimeoutMsg(string RequestId, int DelayMs, int TimeoutMs);

public sealed record AwaitCancelReq(string RequestId, int DelayMs, int CancelAfterMs);

public sealed record AwaitCancelMsg(string RequestId, int DelayMs, int CancelAfterMs);

public sealed record RemoteSpotAwaitReq(string RequestId, string TargetSpotRid, int DelayMs);

public sealed record RemoteSpotAwaitMsg(string RequestId, string TargetSpotRid, int DelayMs);

public sealed record EnsureSpotReq(
    string SpotRid,
    string SpotType = AutomaticTurnDispatchNames.SpotType);

public sealed record EnsureSpotRes(string SpotRid, string NodeRid);

public sealed record PlacementWeightReq(int Weight, bool VerifyLocal = true);

public sealed record PlacementWeightRes(int Weight);

public sealed record ProbeReq(string RequestId, string Marker);

public sealed record ProbeMsg(string RequestId, string Marker);

public sealed record TimerStartReq(string RequestId, string TimerName, string Mode, int PeriodMs, int DelayMs);

public sealed record TimerStartMsg(string RequestId, string TimerName, string Mode, int PeriodMs, int DelayMs);

public sealed record TimerStopReq(string RequestId);

public sealed record TimerStopMsg(string RequestId);

public sealed record ActorAwaitReq(string RequestId, int DelayMs, string Terminator = "async");

public sealed record ActorFastReq(string RequestId, string Marker);

public sealed record PerActorAwaitReq(string RequestId, int DelayMs, string Terminator = "async");

public sealed record PerActorFastReq(string RequestId, string Marker);

public sealed record ActorJoinAwaitReq(string RequestId, string TargetSpotRid);

public sealed record ActorPushAwaitReq(string RequestId, int DelayMs, string Value);

public sealed record ActorPushNotify(string ActorId, string RequestId, string Value, string NodeRid);

public sealed record ActorAwaitRes(
    string ScenarioId,
    string RequestId,
    string ActorId,
    string SpotRid,
    string NodeRid,
    string Marker);

public sealed record AutomaticTurnDispatchRes(
    string ScenarioId,
    string RequestId,
    string SpotRid,
    string NodeRid,
    string Marker);

public sealed record AwaitTimeoutRes(
    string ScenarioId,
    string RequestId,
    string SpotRid,
    string NodeRid,
    bool TimedOut,
    string Error);

public sealed record AwaitCancelRes(
    string ScenarioId,
    string RequestId,
    string SpotRid,
    string NodeRid,
    bool Canceled,
    string Error);

public sealed record AwaitShutdownScenarioRes(
    string Operation,
    string SpotRid,
    string[] Evidence);

public sealed record AwaitShutdownRecoveryRes(
    string Operation,
    string SpotRid,
    string[] Evidence);
