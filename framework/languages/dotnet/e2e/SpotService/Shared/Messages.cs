using Zlink.Framework.Contracts.Handlers;

namespace SpotService.Shared;

public static class SpotServiceNames
{
    public const string SpotChannel = "spot.service";
    public const string SpotMsgTopic = "spot.service.events";
    public const string ControlChannel = "spot.control";
    public const string ExternalSpotChannel = "spot.external.play-a";
    public const string ExternalSpotChannelB = "spot.external.play-b";
    public const string ExternalClientChannel = "spot.external.client";
    public const string ExternalClientServerChannel = "spot.external.cs.client";
    public const string StreamNode = "session-stream";
    public const string TlsStreamNode = "session-stream-tls";
    public const string PlaySpotNode = "play-node";
    public const string SessionSpotNode = "session-node";
    public const string EdgeSpotNode = "edge-node";
    public const string EdgePublisherNode = "edge-publisher-node";
    public const string MultiSpotNodeA = "multi-node-a";
    public const string MultiSpotNodeB = "multi-node-b";
    public const string SpotOnlyMesh = "spot-only.mesh";
    public const string MultiRouteChannelA = "multi-route-a";
    public const string MultiRouteChannelB = "multi-route-b";
    public const string ActorType = "scenario-player";
    public const string UserSpotType = "scenario-user-spot";
    public const string InstanceSpotType = "scenario-instance-spot";
    public const string WeightCapacitySpotType = "scenario-weight-capacity-spot";
    public const string AlternateSpotType = "scenario-alternate-spot";
    public const string SpotOnlyUserSpotType = "spot-only-user-spot";
    public const string MultiSpotTypeA = "multi-spot-a";
    public const string MultiSpotTypeB = "multi-spot-b";
    public const string ActorIdMetadata = "actor-id";
}

public sealed record JoinReq(string Key, string ActorId, string DisplayName, int Level, string[] Tags);

public sealed record JoinRes(string SpotRid, string NodeRid, string ActorId);

public sealed record NodeReadinessWaitReq(string NodeRid, int TimeoutMilliseconds = 10000);

public sealed record NodeReadinessWaitRes(string NodeRid, bool PeerReady);

public sealed record EnsureActorReq(string ActorId, string DisplayName);

public sealed record EnsureActorRes(string ActorId, string NodeRid, ulong Generation);

public sealed record ScenarioActorCreateReq(string DisplayName);

public sealed record ActorManagerProbeReq(string Operation, string ActorId);

public sealed record ActorManagerProbeRes(
    string Operation,
    string State,
    ActorRefRes? Actor);

public sealed record StateReq(string Operation, int Delta);

public sealed record StateRes(string SpotRid, string NodeRid, int Value);

public sealed record InstanceColdRequestReq(string SpotId, string OperationId);

public sealed record InstanceColdRequestRes(
    string SpotId,
    string OperationId,
    bool Succeeded,
    string ErrorKind,
    string NodeRid,
    int Value);

public sealed record InstanceColdSendReq(string SpotId, string OperationId);

public sealed record InstanceColdSendRes(
    string SpotId,
    string OperationId,
    bool Accepted,
    string ErrorKind);

public sealed record InstanceColdRequest(string OperationId);

public sealed record InstanceColdSend(string OperationId);

public sealed record ReservedSpotIdProbeReq(string SpotId);

public sealed record ReservedSpotIdProbeRes(
    string UserSpotErrorKind,
    string InstanceSpotErrorKind,
    long LocationStoreReads,
    long LocationStoreWrites,
    int UserSpotFactoryCalls,
    int InstanceSpotFactoryCalls);

public sealed record StateMsg(string Marker);

public sealed record SpotStateRouteReq(string SpotRid, string Operation, int Delta);

public sealed record SpotStateCommandReq(string SpotRid, string Marker);

public sealed record SpotStateCommandRes(string SpotRid, string Marker, bool Accepted, string[] Evidence);

public sealed record EvidenceWaitReq(
    string[] ContainsAll,
    int TimeoutMilliseconds = 10000);

public sealed record PlacementWeightReq(int Weight);

public sealed record PlacementWeightRes(int Weight);

public sealed record PlacementWeightProbeRes(
    int Requested,
    int Current,
    bool Accepted,
    string ErrorKind);

public sealed record TypedSpotCreateReq(string SpotId, string SpotType);

public sealed record SpotMissingHandlerReq(string SpotRid);

public sealed record SpotMissingHandlerRes(string SpotRid, bool Failed, string[] Evidence);

public sealed record SpotMissingCommandReq(string SpotRid, string Marker);

public sealed record SpotMissingCommandRes(string SpotRid, string Marker, bool Sent, string[] Evidence);

public sealed record SpotMissingTargetReq(string SpotRid);

public sealed record SpotMissingTargetRes(string SpotRid, bool Failed);

public sealed record StageProbeReq(string Marker, int Delta);

public sealed record StageTimerStartMsg(string Name, int PeriodMs);

public sealed record SpotStageProbeReq(string SpotRid, string Marker, int Delta);

public sealed record SpotStageTimerReq(string SpotRid, string Name, int PeriodMs);

public sealed record SpotStageTimerRes(string SpotRid, string Name, bool Started, string[] Evidence);

public sealed record SpotMsg(string Marker);

public sealed record SpotBackpressureMsg(string Marker, int Sequence, string Payload);

public sealed record SpotBackpressurePublishReq(
    string Marker,
    int PayloadBytes = 65536,
    int MaxAttempts = 20000,
    bool Blocking = false,
    int StartSequence = 1);

public sealed record SpotBackpressureSubmitRes(
    int Sequence,
    long ElapsedMilliseconds);

public sealed record SpotBackpressureAttemptRes(
    int Sequence,
    string Status,
    long ElapsedMilliseconds,
    ulong SnapshotRemoteNodeCount,
    ulong AdmittedRemoteNodeCount,
    ulong DroppedRemoteNodeCount,
    ulong SnapshotLocalSpotCount,
    ulong AdmittedLocalSpotCount,
    ulong DroppedLocalSpotCount);

public sealed record SpotBackpressurePublishRes(
    SpotBackpressureAttemptRes NonBlocking,
    SpotBackpressureAttemptRes Blocking);

public sealed record SpotOutboundMsg(string Marker);

public sealed record SpotOutboundNegativeMsg(string Marker);

public sealed record SpotOutboundRouteReq(string SpotRid, string Marker);

public sealed record SpotOutboundRouteRes(string SpotRid, string Marker, bool Accepted, string[] Evidence);

public sealed record ChannelEchoReq(string Value);

public sealed record ChannelEchoRes(string Value);

public sealed record ChannelNotify(string Marker);

public sealed record MissingChannelReq(string Value);

public sealed record MissingChannelNotify(string Marker);

public sealed record CreateSpotReq(string SpotRid);

public sealed record CreateSpotRes(string SpotRid, string NodeRid, string State);

public sealed record CloseSpotReq(string SpotRid);

public sealed record CloseSpotRes(string SpotRid, bool Closed);

public sealed record JoinUserSpotActorReq(string SpotRid, string ActorId);

public sealed record JoinUserSpotActorRes(string SpotRid, string ActorId, bool Accepted, ulong Generation);

public sealed record JoinAdmittedUserSpotActorReq(string SpotRid, string ActorId, bool Allow, string Reason);

public sealed record JoinAdmittedUserSpotActorRes(
    string SpotRid,
    string ActorId,
    bool Accepted,
    ulong Generation,
    string ErrorKind);

public sealed record ActorPingReq(string Value);

[ZLinkPacket("UserActorPingReq")]
public sealed record UserActorPingReq(string Value);

public sealed record SlowActorPingReq(string Value, int DelayMs);

public sealed record ActorPingRes(string ActorId, string NodeRid, string SpotRid, string Value, int Seen);

public sealed record ActorPushReq(string Value);

public sealed record ActorPushNotify(string ActorId, string Value, int Seen);

public sealed record ActorPushByActorReq(string ActorId, string Value);

public sealed record ActorMissingWaitReq(string ActorId, int TimeoutMilliseconds = 10000);

public sealed record ActorPushByActorRes(string ActorId, string Value, bool Delivered, string ErrorKind);

public sealed record ActorRefReq(string ActorId);

public sealed record ActorRefRes(string ActorId, string NodeRid, ulong Generation);

public sealed record ActorRequestReq(
    string ActorId,
    string Value,
    int DelayMilliseconds = 0,
    int TimeoutMilliseconds = 3000);

public sealed record ActorRequestRes(
    bool Succeeded,
    string ErrorKind,
    ActorPingRes? Reply);

public sealed record ActorRefDestroyReq(ActorRefRes Actor);

public sealed record ActorRefDestroyRes(
    bool Succeeded,
    bool Destroyed,
    string ErrorKind);

public sealed record ComplexActorReq(
    string DisplayName,
    int Level,
    string[] Tags,
    Dictionary<string, string> Attributes);

public sealed record ComplexActorRes(
    string ActorId,
    string DisplayName,
    int Level,
    string[] Tags,
    Dictionary<string, string> Attributes);

public sealed record AuthReq(string ActorId, string DisplayName);

public sealed record UserSpotAuthReq(string SpotRid, string ActorId, string DisplayName);

public sealed record AuthRes(string ActorId, string NodeRid);

public sealed record MultiBindReq(string FirstActorId, string SecondActorId);

public sealed record MultiBindRes(int BoundCount);

public sealed record StaleBindingProbeReq(string ActorId, string Value);

public sealed record StaleBindingProbeRes(
    string ActorId,
    bool RelayRejected,
    string ErrorKind,
    bool DisconnectCompleted);

public sealed record LocationStoreReadProbeReq(
    string[] ActorIds,
    bool Blocked);

public sealed record LocationStoreReadProbeSnapshot(
    long MatchingReads,
    bool Blocked,
    string[] ActorIds);

public sealed record NotifyBoundActorDisconnectedReq(string ActorId);

public sealed record NotifyBoundActorDisconnectedRes(string ActorId, bool Completed);

public sealed record LeaveReq(string ActorId);

public sealed record LeaveRes(string ActorId, bool Accepted);

public sealed record SnapshotReq(string ActorId);

public sealed record SnapshotRes(string ActorId, int Seen);

public sealed record ControlPingReq(string Value);

public sealed record ControlPingRes(string Value, string NodeRid);

public sealed record SpotTypeMismatchReq(string SpotRid);

public sealed record SpotTypeMismatchRes(string SpotRid, bool Failed, string ErrorKind, string State);

public sealed record WorkerStartReq(string Marker, int DelayMs);

public sealed record WorkerStartRes(string SpotRid, string NodeRid, string Marker);

public sealed record SpotWorkerStartReq(string SpotRid, string Marker, int DelayMs);

public sealed record SpotWorkerCompleteReq(string SpotRid, string Marker);

public sealed record SpotWorkerCompleteRes(string SpotRid, string Marker, bool Completed, string[] Evidence);

public sealed record SlowSpotReq(string Marker, int DelayMs);

public sealed record SlowSpotRes(string SpotRid, string NodeRid, string Marker);

public sealed record SpotSlowRouteReq(string SpotRid, string Marker, int DelayMs, int TimeoutMs);

public sealed record SpotSlowRouteRes(string SpotRid, string Marker, bool TimedOut);

public sealed record DestroyActorReq(string ActorId);

public sealed record DestroyActorRes(string ActorId, bool Destroyed);

public sealed record OverrunStartMsg(string Name, string Policy, int PeriodMs);

public sealed record SpotOverrunStartReq(string SpotRid, string Name, string Policy, int PeriodMs);

public sealed record SpotOverrunStartRes(string SpotRid, string Name, string Policy, bool Started, string[] Evidence);

public sealed record TimerStartMsg(string Name, int PeriodMs);

public sealed record IdleCloseMsg(string Name, int PeriodMs);

public sealed record SpotTimerStartReq(string SpotRid, string Name, int PeriodMs);

public sealed record SpotTimerStartRes(string SpotRid, string Name, bool Started, string[] Evidence);

public sealed record SpotIdleCloseReq(string SpotRid, string Name, int PeriodMs);

public sealed record SpotIdleCloseRes(string SpotRid, string Name, bool Closed, string[] Evidence);

public sealed record SpotToSpotReq(string TargetSpotRid, string Marker);

public sealed record SpotToSpotRes(string SourceSpotRid, string TargetSpotRid, int TargetValue);

public sealed record SpotToSpotRouteReq(string SourceSpotRid, string TargetSpotRid, string Marker);

public sealed record SpotToSpotTimeoutReq(string TargetSpotRid, string Marker);

public sealed record SpotToSpotTimeoutRes(string SourceSpotRid, string TargetSpotRid, bool Failed);

public sealed record SpotToSpotTimeoutRouteReq(string SourceSpotRid, string TargetSpotRid, string Marker);

public sealed record SpotToSpotNegativeReq(string TargetSpotRid, string Marker);

public sealed record SpotToSpotNegativeRes(string SourceSpotRid, string TargetSpotRid, bool RequestFailed);

public sealed record SpotToSpotNegativeRouteReq(string SourceSpotRid, string TargetSpotRid, string Marker);

public sealed record MissingSpotReq(string Value);

public sealed record MissingSpotMsg(string Marker);

public sealed record MultiNodeCreateSpotReq(string SpotRid, int Delta);

public sealed record MultiNodeCreateSpotRes(string SpotRid, string NodeRid, string State, int Value);

public sealed record MultiNodeStateRouteReq(string SpotRid, int Delta);

public sealed record SpotOnlyMeshReq(string SourceSpotRid, string TargetSpotRid, string Marker);

public sealed record SpotOnlyMeshRes(string SourceSpotRid, string TargetSpotRid, int TargetValue, string Marker);

public sealed record SpotOnlyJoinReq(string TargetSpotRid, string ActorId, string Marker);

public sealed record SpotOnlyJoinRes(string TargetSpotRid, string ActorId, bool Accepted, string Marker);

public sealed record SpotPublishReq(string SpotRid, string Marker);

public sealed record SpotPublishRes(
    string Operation,
    string PublisherRid,
    string SpotRid,
    string Marker,
    string[] Evidence);

public sealed record SpotPublishObserveRes(
    string Operation,
    string SpotRid,
    string Marker,
    bool Received,
    string[] Evidence);
