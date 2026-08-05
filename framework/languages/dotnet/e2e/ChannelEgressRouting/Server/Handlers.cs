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
    : IZLinkRequestHandler<ChannelProbeRequest, ChannelProbeReply>
{
    public async ValueTask<ChannelProbeReply> HandleAsync(
        ChannelProbeRequest request,
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
                    new ChannelObjectProbeRequest($"{request.Id}-spot"))
                .Async<ChannelObjectProbeReply>(cancellationToken);
            downstream.Add($"spot:{spotReply.SpotId}:{spotReply.NodeRid}");
            var actorReply = await actors
                .RequestToActor(
                    actorId,
                    new ChannelObjectProbeRequest($"{request.Id}-actor"))
                .Async<ChannelObjectProbeReply>(cancellationToken);
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
                    new ChannelProbeRequest($"{request.Id}-audit"))
                .Async<ChannelProbeReply>(cancellationToken);
            downstream.Add($"{audit.Role}:{audit.Channel}");

            var workflow = await routes
                .RequestToChannel(
                    ChannelEgressNames.Workflow,
                    new ChannelProbeRequest($"{request.Id}-workflow"))
                .Async<ChannelProbeReply>(cancellationToken);
            downstream.Add($"{workflow.Role}:{workflow.Channel}");
        }

        return new ChannelProbeReply(
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
    : IZLinkSendHandler<ChannelProbeCommand>
{
    public ValueTask HandleAsync(
        ChannelProbeCommand message,
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
