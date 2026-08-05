namespace RuntimeMonitoring.Shared;

public static class RuntimeMonitoringNames
{
    public const string Channel = "monitor.profile";
    public const string LocationRuntimeSource = "location-runtime";
    public const string ChannelServerSource = "monitor.profile.server";
    public const string ChannelClientSource = "monitor.profile.client";
    public const string SpotChannel = "monitor.spot";
    public const string SubjectSpotType = "monitor.subject";
}

public sealed record ProfileReq(string Value, string Marker);

public sealed record ProfileRes(string Value, string ProviderRid, string Marker);

public sealed record DrainResultRes(string Result, string? Reason = null);

public sealed record MeshRuntimeSnapshotRes(
    string MeshName,
    string State,
    bool IsReady,
    int ReadyPeerCount,
    ulong Sequence,
    DateTimeOffset ObservedAt,
    MeshRuntimePeerRes[] Peers,
    MeshRuntimeChannelRes[] Channels,
    MeshRuntimePlacementRes Placement);

public sealed record MeshRuntimePeerRes(
    string Rid,
    string State,
    string? UnavailableReason);

public sealed record MeshRuntimeChannelRes(
    string ChannelName,
    bool IsReady,
    int ReadyTargetCount);

public sealed record MeshRuntimePlacementRes(
    bool IsAvailable,
    int ActiveActorCount,
    int ActiveSpotCount,
    string? UnavailableReason);

public sealed record EvidenceWaitReq(
    string[] ContainsAll,
    string[][] ContainsAnyGroups,
    int TimeoutMilliseconds = 10000,
    int AfterIndex = 0);

public sealed record ObserverIsolationStatusRes(
    bool Running,
    bool SlowConsumerReleased,
    bool SlowConsumerFailed,
    int NormalEventCount,
    ulong NormalLatestSequence,
    ulong SlowLatestSequence,
    bool NormalSequenceGapObserved);

public sealed record RuntimeValidationRes(
    bool MissingSnapshotRejected,
    bool MissingObserverRejected,
    bool RegisteredObserverProducedStatus);
