using ChannelEgressRouting.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace ChannelEgressRouting.Server;

internal sealed class ChannelProbeRequestHandler(
    RoleOptions options,
    EvidenceStore evidence,
    IZLinkRouteClient routes,
    IZLinkActorClient actors,
    IZLinkSpotClient spots)
    : IZLinkRequestHandler<ChannelProbeReq, ChannelProbeRes>
{
    public async ValueTask<ChannelProbeRes> HandleAsync(
        ChannelProbeReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        var channel = context.ChannelName ?? "<none>";
        evidence.Add($"request|role={options.Role}|channel={channel}|id={request.Id}");
        var downstream = new List<string>();
        if (options.WorkflowServer
            && channel == ChannelEgressNames.Workflow
            && request.Mode.StartsWith("state-address:", StringComparison.Ordinal))
        {
            var parts = request.Mode.Split(':', 3);
            if (parts.Length != 3)
                throw new InvalidOperationException("State address mode is invalid.");
            var actorId = parts[1];
            var spotId = parts[2];
            var spotReply = await spots
                .RequestToSpot(
                    spotId,
                    new ChannelObjectProbeReq($"{request.Id}-spot"))
                .Async<ChannelObjectProbeRes>(cancellationToken);
            downstream.Add($"spot:{spotReply.SpotId}:{spotReply.NodeRid}");
            var actorReply = await actors
                .RequestToActor(
                    actorId,
                    new ChannelObjectProbeReq($"{request.Id}-actor"))
                .Async<ChannelObjectProbeRes>(cancellationToken);
            downstream.Add(
                $"actor:{actorReply.ActorId}:{actorReply.SpotId}:{actorReply.NodeRid}");
        }
        if (options.Role == "play"
            && channel == ChannelEgressNames.Play
            && request.Mode == "cascade")
        {
            var audit = await routes
                .RequestToChannel(
                    ChannelEgressNames.Audit,
                    new ChannelProbeReq($"{request.Id}-audit"))
                .Async<ChannelProbeRes>(cancellationToken);
            downstream.Add($"{audit.Role}:{audit.Channel}");

            var workflow = await routes
                .RequestToChannel(
                    ChannelEgressNames.Workflow,
                    new ChannelProbeReq($"{request.Id}-workflow"))
                .Async<ChannelProbeRes>(cancellationToken);
            downstream.Add($"{workflow.Role}:{workflow.Channel}");
        }

        return new ChannelProbeRes(
            request.Id,
            options.Role.StartsWith("workflow", StringComparison.Ordinal)
                ? options.Rid
                : options.Role,
            channel,
            downstream.ToArray());
    }
}

internal sealed class ChannelProbeCommandHandler(
    RoleOptions options,
    EvidenceStore evidence)
    : IZLinkSendHandler<ChannelProbeMsg>
{
    public ValueTask HandleAsync(
        ChannelProbeMsg message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"send|role={options.Role}|channel={context.ChannelName ?? "<none>"}|id={message.Id}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class FanoutProbeHandler(
    RoleOptions options,
    EvidenceStore evidence)
    : IZLinkFanoutHandler<FanoutProbeEvent>
{
    public ValueTask HandleAsync(
        FanoutProbeEvent message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"fanout|role={options.Role}|id={message.Id}");
        return ValueTask.CompletedTask;
    }
}
