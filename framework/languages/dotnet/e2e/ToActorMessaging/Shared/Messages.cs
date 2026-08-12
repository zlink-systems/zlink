namespace ToActorMessaging.Shared;

public sealed record ActorMsg(string Scenario, string ActorId, string Value);

public sealed record ActorReq(string Scenario, string ActorId, string Value);

public sealed record ActorRes(string Scenario, string ActorId, string Value);

public sealed record ActorEvidence(
    string Scenario,
    string ActorId,
    string Kind,
    string Value,
    string? NodeRid = null,
    ulong? Generation = null,
    string? PacketName = null,
    string? RequestId = null);

public sealed record ActorCallReq(
    string Scenario,
    string ActorId,
    string Value,
    string? TargetNodeRid = null,
    ulong? TargetGeneration = null);

public sealed record ActorCallRes(string Scenario, string ActorId, string Result, string? ErrorKind = null);

public sealed record DestroyActorReq(string ActorId, string Scenario);

public sealed record DestroyActorRes(string ActorId, bool Destroyed);

public sealed record BindActorReq(string ActorId);

public sealed record BindActorRes(string ActorId, string NodeRid, ulong Generation);

public sealed record BoundPushReq(string Scenario, string ActorId, string Value);

public sealed record BoundPushRes(
    string ActorId,
    string Value,
    bool Submitted,
    string? ErrorKind = null);

public sealed record BoundPushNotify(string Scenario, string ActorId, string Value);

public sealed record ActorRouteStatus(string ActorId, bool Exists);

public sealed record BoundSessionSnapshot(
    string ActorId,
    string? SessionRid,
    long Revision);
