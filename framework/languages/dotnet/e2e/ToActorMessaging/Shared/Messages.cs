namespace ToActorMessaging.Shared;

public sealed record ActorNotify(string Scenario, string ActorId, string Value);

public sealed record ActorAsk(string Scenario, string ActorId, string Value);

public sealed record ActorReply(string Scenario, string ActorId, string Value);

public sealed record ActorEvidence(
    string Scenario,
    string ActorId,
    string Kind,
    string Value,
    string? NodeRid = null,
    ulong? Generation = null,
    string? PacketName = null,
    string? RequestId = null);

public sealed record ActorCallRequest(
    string Scenario,
    string ActorId,
    string Value,
    string? TargetNodeRid = null,
    ulong? TargetGeneration = null);

public sealed record ActorCallResponse(string Scenario, string ActorId, string Result, string? ErrorKind = null);

public sealed record DestroyActorRequest(string ActorId, string Scenario);

public sealed record DestroyActorReply(string ActorId, bool Destroyed);

public sealed record BindActorRequest(string ActorId);

public sealed record BindActorReply(string ActorId, string NodeRid, ulong Generation);

public sealed record BoundPushRequest(string Scenario, string ActorId, string Value);

public sealed record BoundPushReply(
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
