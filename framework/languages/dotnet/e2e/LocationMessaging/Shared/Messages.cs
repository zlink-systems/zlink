namespace LocationMessaging.Shared;

public sealed record ProfileReq(string Value);

public sealed record ProfileRes(string Value, string ProviderRid);

public sealed record ProfileMsg(string CommandId);

public sealed record ProfileEvent(string CommandId);

public sealed record BackpressureMsg(string CommandId, string Payload);

public sealed record MissingProfileReq(string Value);

public sealed record MissingProfileMsg(string CommandId);

public sealed record PeerLocationRow(
    string MeshName,
    string? NodeRid,
    string Role,
    string Endpoint,
    bool Draining,
    string State);

public sealed record PeerLocationWaitReq(
    string MeshName,
    string Role,
    string NodeRid,
    bool Present,
    string? Endpoint = null,
    int TimeoutMilliseconds = 30000);

public sealed record LocationStatusRes(
    bool StoreHealthy,
    bool OwnerLeaseHealthy);

public sealed record EvidenceWaitReq(
    string Contains,
    int TimeoutMilliseconds = 10000,
    int AfterCount = 0);

public sealed record EvidenceCountWaitReq(
    string Contains,
    int MinimumCount,
    int TimeoutMilliseconds = 10000);

public sealed record EvidenceQuietWaitReq(
    string Contains,
    int QuietMilliseconds = 1500,
    int TimeoutMilliseconds = 30000);

public sealed record PayloadReq(string Marker, string Payload);

public sealed record PayloadRes(string Marker, int Length, string Sha256);

public sealed record WorkflowReq(string Value);

public sealed record WorkflowRes(string Value, string ProviderRid);

public sealed record ScenarioRouteReq(string Value);

public sealed record TargetedRouteReq(string TargetRid, string Value);

public sealed record ScenarioRouteRes(
    string Value,
    string ProviderRid,
    string SourceRid);

public sealed record ExpectedFailureRes(string ErrorKind);

public sealed record DrainResultRes(string Result, string? Reason = null);

public sealed record RequestOutcomeRes(string Value, string Outcome);
