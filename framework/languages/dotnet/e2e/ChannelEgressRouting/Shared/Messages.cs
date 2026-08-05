using Zlink.Framework.Contracts.Handlers;

namespace ChannelEgressRouting.Shared;

public static class ChannelEgressNames
{
    public const string GameMesh = "game";
    public const string AuditMesh = "audit";
    public const string Session = "game.session";
    public const string Play = "game.play";
    public const string Api = "game.api";
    public const string Audit = "audit.record";
    public const string Workflow = "workflow.command";
}

[ZLinkPacket("ChannelProbeRequest")]
public sealed record ChannelProbeRequest(string Id, string Mode = "echo");

public sealed record ChannelProbeReply(
    string Id,
    string Role,
    string Channel,
    string[] Downstream);

[ZLinkPacket("ChannelProbeCommand")]
public sealed record ChannelProbeCommand(string Id);

[ZLinkPacket("FanoutProbeEvent")]
public sealed record FanoutProbeEvent(string Id);

[ZLinkPacket("LogicalMulticastProbeEvent")]
public sealed record LogicalMulticastProbeEvent(string Id);

public sealed record RouteInvokeRequest(
    string Channel,
    string Id,
    string Mode = "echo");

public sealed record RouteInvokeResult(
    bool Succeeded,
    string? Error,
    ChannelProbeReply? Reply,
    long ElapsedMilliseconds);

public sealed record SendInvokeResult(
    bool Succeeded,
    string? Error,
    long ElapsedMilliseconds);

[ZLinkPacket("ChannelObjectProbeRequest")]
public sealed record ChannelObjectProbeRequest(string Id);

public sealed record ChannelObjectProbeReply(
    string Id,
    string ActorId,
    string SpotId,
    int StateVersion,
    string NodeRid);

[ZLinkPacket("ChannelSpotWorkflowRequest")]
public sealed record ChannelSpotWorkflowRequest(string Id);

public sealed record ChannelSpotWorkflowReply(
    string Id,
    int StateVersion,
    string WorkflowRole);

[ZLinkPacket("ChannelActorJoinRequest")]
public sealed record ChannelActorJoinRequest(string Id, string TargetSpotId);

public sealed record ChannelActorJoinReply(
    string Id,
    string ActorId,
    string TargetSpotId,
    bool Submitted);

public sealed record ChannelActorCreateRequest(string ActorId);

public sealed record ChannelActorCreateReply(
    string ActorId,
    string NodeRid,
    ulong Generation);

public sealed record ChannelSpotCreateRequest(string SpotId);

public sealed record ChannelSpotCreateReply(
    string SpotId,
    string NodeRid,
    ulong Generation);

public sealed record ChannelObjectScenarioRequest(
    string ActorId,
    string SpotId,
    string Id);

public sealed record ChannelObjectScenarioReply(
    ChannelObjectProbeReply Actor,
    ChannelObjectProbeReply Spot);

[ZLinkPacket("ChannelBindActorRequest")]
public sealed record ChannelBindActorRequest(string ActorId);

public sealed record ChannelBindActorReply(
    string ActorId,
    string NodeRid,
    ulong Generation);

[ZLinkPacket("ChannelBoundPushRequest")]
public sealed record ChannelBoundPushRequest(string Id);

public sealed record ChannelBoundPushReply(string Id, bool Submitted);

[ZLinkPacket("ChannelBoundPushNotification")]
public sealed record ChannelBoundPushNotification(
    string Id,
    string ActorId,
    string SpotId,
    int StateVersion);
