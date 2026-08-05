namespace StoreFailure.Shared;

public static class StoreFailureNames
{
    public const string Channel = "storefailure.profile";

    // Callers are mesh members like any other node (spec 10 §1); they carry a
    // distinct ChannelName so profile select-one never targets a consumer.
    public const string ConsumerChannel = "store-failure.consumer";
}

public sealed record ProfileReq(string Value, string Marker);

public sealed record ProfileRes(string Value, string ProviderRid, string Marker);

public sealed record ProfileMsg(string Marker);

public sealed record EvidenceWaitReq(
    string[] ContainsAll,
    string[][] ContainsAnyGroups,
    int TimeoutMilliseconds = 10000);

public sealed record WeightWaitReq(
    int Expected,
    int TimeoutMilliseconds = 10000);

public sealed record StoreDelayReq(int Milliseconds);

public sealed record TopologyWaitReq(
    string RoutingId,
    string State,
    int ExpectedCount,
    int TimeoutMilliseconds = 30000);

public sealed record RegistryHealthWaitReq(
    bool ExpectedHealthy,
    int TimeoutMilliseconds = 30000);

public sealed record PeerRowsWaitReq(
    string[] PresentRids,
    string[] AbsentRids,
    string[] DrainingRids,
    int TimeoutMilliseconds = 30000);

public sealed record RouteReadyWaitReq(
    int MinimumReadyMembers,
    string[] ReadyRids,
    string[] NotReadyRids,
    int TimeoutMilliseconds = 10000);

public sealed record RouteReadyRes(int ReadyMemberCount);

public sealed record RuntimeStatusWaitReq(
    bool? StoreHealthy,
    bool? OwnerLeaseHealthy,
    bool RequireLastRefresh = false,
    int TimeoutMilliseconds = 30000,
    DateTimeOffset? LastRefreshAfter = null);

public sealed record RuntimeStatusRes(
    bool StoreHealthy,
    bool OwnerLeaseHealthy,
    DateTimeOffset? OwnerLeaseRenewedAt,
    DateTimeOffset? LastRefreshAt);

public sealed record PeerRowRes(string? Rid, string Endpoint, bool Draining);

public sealed record DrainResultRes(string Result, string? Reason);
