using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.ZoneNode.Application.Node;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Handlers;

/// <summary>
/// The world announcement (§8.2). Ops publishes it to a fanout channel without holding a
/// node list, so adding a node changes nothing on the publishing side.
///
/// The announcement is then handed to configured zone Spot IDs with direct sends. Publish
/// completion does not report subscriber delivery, and each direct send waits only for
/// source-local outbound admission.
/// </summary>
[ZLinkHandlerGroup(HandlerGroups.ZoneBroadcast)]
internal sealed class WorldAnnounceSubscriber(
    IZLinkSpotClient routes,
    NodeMaintenancePolicy maintenance,
    ILogger<WorldAnnounceSubscriber> logger)
    : IZLinkFanoutHandler<WorldAnnounceEvent>
{
    public async ValueTask HandleAsync(
        WorldAnnounceEvent message,
        CancellationToken cancellationToken)
    {
        var zones = ZoneTopology.ZonesOf(maintenance.OwnNodeId);
        logger.LogInformation(
            "fanout subscriber received announcement. node={NodeId}, announcement={AnnouncementId}, zones={ZoneCount}",
            maintenance.OwnNodeId,
            message.AnnouncementId,
            zones.Count);

        foreach (var zoneId in zones)
        {
            // One zone that cannot be reached must not take the others down with it, and it must
            // not pass unnoticed either: an announcement that silently reaches half the world
            // looks exactly like one that reached all of it.
            try
            {
                // Async waits only for transport admission, not handler execution. Awaiting
                // it keeps an admission failure visible without extending the remote handler turn.
                await routes
                    .SendToSpot(zoneId, new DeliverAnnounceMsg(message.AnnouncementId, message.Text))
                    .Async(cancellationToken);
            }
            catch (Exception error)
            {
                logger.LogError(
                    error,
                    "announcement dropped: delivering to the node's own zone spot failed. zone={ZoneId}",
                    zoneId);
            }
        }
    }
}

/// <summary>
/// The subscriber of the node that hosts no zone (§11.1). There is nowhere to deliver an
/// announcement to, and that is the point: receiving one is the evidence that Ops published
/// to a node it was never configured with, without a line of Ops changing (ZW-D2).
/// </summary>
[ZLinkHandlerGroup(HandlerGroups.BroadcastProbe)]
internal sealed class BroadcastProbeSubscriber(
    NodeMaintenancePolicy maintenance,
    ILogger<BroadcastProbeSubscriber> logger)
    : IZLinkFanoutHandler<WorldAnnounceEvent>
{
    public ValueTask HandleAsync(
        WorldAnnounceEvent message,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "fanout subscriber received announcement. node={NodeId}, announcement={AnnouncementId}, zones={ZoneCount}",
            maintenance.OwnNodeId,
            message.AnnouncementId,
            0);
        return ValueTask.CompletedTask;
    }
}

/// <summary>
/// Keeps this node's view of every node's maintenance state current (§2.3). The node
/// judging a move is the one the player is leaving, so it needs the target node's state;
/// reading the store on every move would be expensive, so the state arrives by fanout.
/// </summary>
[ZLinkHandlerGroup(HandlerGroups.ZoneBroadcast)]
internal sealed class NodeMaintenanceChangedSubscriber(
    NodeMaintenancePolicy maintenance,
    ILogger<NodeMaintenanceChangedSubscriber> logger)
    : IZLinkFanoutHandler<NodeMaintenanceChangedEvent>
{
    public ValueTask HandleAsync(
        NodeMaintenanceChangedEvent message,
        CancellationToken cancellationToken)
    {
        maintenance.Apply(message.NodeId, message.Enabled);
        logger.LogInformation(
            "maintenance cache updated. observer={ObserverNodeId}, node={NodeId}, enabled={Enabled}",
            maintenance.OwnNodeId,
            message.NodeId,
            message.Enabled);
        return ValueTask.CompletedTask;
    }
}
