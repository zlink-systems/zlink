using Zlink.Framework.Contracts.Channels;
using ZoneWorld.Server.Ops.Ports;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Ops.Infrastructure.ZLink;

internal sealed class WorldOperationsAdapter(
    IZLinkFanoutClient fanout) : IWorldOperationsPort
{
    public async ValueTask PublishAnnouncementAsync(
        string announcementId,
        string text,
        CancellationToken cancellationToken) =>
        await fanout
            .Publish(
                ZoneWorldNames.BroadcastChannel,
                ZoneWorldNames.AnnounceTopic,
                new WorldAnnounceEvent(announcementId, text))
            .Async(cancellationToken);

    // --8<-- [start:doc-zw-ops-publish]
    public async ValueTask PublishMaintenanceChangeAsync(
        string nodeId,
        bool enabled,
        CancellationToken cancellationToken) =>
        await fanout
            .Publish(
                ZoneWorldNames.BroadcastChannel,
                ZoneWorldNames.MaintenanceTopic,
                new NodeMaintenanceChangedEvent(nodeId, enabled))
            .Async(cancellationToken);
    // --8<-- [end:doc-zw-ops-publish]
}
