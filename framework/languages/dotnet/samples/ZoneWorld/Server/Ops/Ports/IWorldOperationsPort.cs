using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Ops.Ports;

/// <summary>
/// Operations that leave the Ops application boundary. Implementations hide fanout topics
/// from the use cases.
/// </summary>
public interface IWorldOperationsPort
{
    ValueTask PublishAnnouncementAsync(
        string announcementId,
        string text,
        CancellationToken cancellationToken);

    ValueTask PublishMaintenanceChangeAsync(
        string nodeId,
        bool enabled,
        CancellationToken cancellationToken);
}
