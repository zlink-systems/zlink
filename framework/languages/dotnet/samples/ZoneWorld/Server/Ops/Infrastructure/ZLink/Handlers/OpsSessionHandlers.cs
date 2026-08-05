using Zlink.Framework.Contracts.Streams;
using ZoneWorld.Server.Ops.Application.Ops;
using ZoneWorld.Server.Ops.Infrastructure.ZLink.Sessions;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Ops.Infrastructure.ZLink.Handlers;

internal sealed class WatchNodesHandler(NodeRegistry nodes, OpsConsoleRegistry consoles)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, WatchNodesReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        WatchNodesReq request,
        CancellationToken cancellationToken)
    {
        await context.Client.Reply(new WatchNodesRes(nodes.Snapshot()))
            .Async(cancellationToken);
        await consoles.ReplayNodesAsync(context, cancellationToken);
        await consoles.ReplayAlertsAsync(context, cancellationToken);
    }
}

/// <summary>
/// Publishes an announcement to every node without knowing how many there are (§8.2).
/// The fanout is the point: a third node can be started and it receives this with no
/// change to Ops.
/// </summary>
internal sealed class AnnounceWorldHandler(
    AnnouncementService announcements,
    ILogger<AnnounceWorldHandler> logger)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, AnnounceWorldReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        AnnounceWorldReq request,
        CancellationToken cancellationToken)
    {
        var announcementId = await announcements.PublishAsync(request.Text, cancellationToken);

        logger.LogInformation(
            "announcement published. announcement={AnnouncementId}",
            announcementId);

        await context.Client.Reply(new AnnounceWorldRes(announcementId))
            .Async(cancellationToken);
    }
}

/// <summary>
/// Switches one node into maintenance (§8.4). The desired state is written first, so a
/// node that is currently down still picks the change up when it restarts. The call to the
/// node itself goes to the channel named after that node, so it reaches that node and no
/// other — a plain client-server channel would spread it across peers.
/// </summary>
internal sealed class SetMaintenanceHandler(
    MaintenanceService maintenance)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, SetMaintenanceReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        SetMaintenanceReq request,
        CancellationToken cancellationToken)
    {
        var reply = await maintenance.SetAsync(request.NodeId, request.Enabled, cancellationToken);
        await context.Client.Reply(reply).Async(cancellationToken);
    }
}

internal sealed class NodeDiagnosticsHandler(
    NodeDiagnosticsService diagnostics)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, NodeDiagnosticsReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        NodeDiagnosticsReq request,
        CancellationToken cancellationToken)
    {
        var reply = await diagnostics.GetAsync(request.NodeId, cancellationToken);
        await context.Client.Reply(reply).Async(cancellationToken);
    }
}
