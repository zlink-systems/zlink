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

[ZLinkPacket("ChannelProbeReq")]
public sealed record ChannelProbeReq(string Id, string Mode = "echo");

public sealed record ChannelProbeRes(
    string Id,
    string Role,
    string Channel,
    string[] Downstream);

[ZLinkPacket("ChannelProbeMsg")]
public sealed record ChannelProbeMsg(string Id);

[ZLinkPacket("FanoutProbeEvent")]
public sealed record FanoutProbeEvent(string Id);

[ZLinkPacket("LogicalMulticastProbeEvent")]
public sealed record LogicalMulticastProbeEvent(string Id);

public sealed record RouteRequestInvokeReq(
    string Channel,
    string Id,
    string Mode = "echo");

public sealed record RouteRequestInvokeRes(
    bool Succeeded,
    string? Error,
    ChannelProbeRes? Reply,
    long ElapsedMilliseconds);

public sealed record RouteSendInvokeReq(
    string Channel,
    string Id);

public sealed record RouteSendInvokeRes(
    bool Succeeded,
    string? Error,
    long ElapsedMilliseconds);

[ZLinkPacket("ChannelObjectProbeReq")]
public sealed record ChannelObjectProbeReq(string Id);

public sealed record ChannelObjectProbeRes(
    string Id,
    string ActorId,
    string SpotId,
    int StateVersion,
    string NodeRid);

[ZLinkPacket("ChannelSpotWorkflowReq")]
public sealed record ChannelSpotWorkflowReq(string Id);

public sealed record ChannelSpotWorkflowRes(
    string Id,
    int StateVersion,
    string WorkflowRole);

[ZLinkPacket("ChannelActorJoinReq")]
public sealed record ChannelActorJoinReq(string Id, string TargetSpotId);

public sealed record ChannelActorJoinRes(
    string Id,
    string ActorId,
    string TargetSpotId,
    bool Submitted);

public sealed record ChannelActorCreateReq(string ActorId);

public sealed record ChannelActorCreateRes(
    string ActorId,
    string NodeRid,
    ulong Generation);

public sealed record ChannelSpotCreateReq(string SpotId);

public sealed record ChannelSpotCreateRes(
    string SpotId,
    string NodeRid,
    ulong Generation);

public sealed record ChannelObjectScenarioReq(
    string ActorId,
    string SpotId,
    string Id);

[ZLinkPacket("ChannelBindActorReq")]
public sealed record ChannelBindActorReq(string ActorId);

public sealed record ChannelBindActorRes(
    string ActorId,
    string NodeRid,
    ulong Generation);

[ZLinkPacket("ChannelBoundPushReq")]
public sealed record ChannelBoundPushReq(string Id);

public sealed record ChannelBoundPushRes(string Id, bool Submitted);

[ZLinkPacket("ChannelBoundPushNotify")]
public sealed record ChannelBoundPushNotify(
    string Id,
    string ActorId,
    string SpotId,
    int StateVersion);
