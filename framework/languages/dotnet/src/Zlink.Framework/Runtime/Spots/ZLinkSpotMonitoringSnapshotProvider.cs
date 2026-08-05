namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotMonitoringSnapshotProvider(IZLinkBackendSpotNode node)
{
    public ZLinkSpotMonitoringSnapshot MonitorStatus()
    {
        return new ZLinkSpotMonitoringSnapshot(
            node.Status(),
            node.Peers()
                .OrderBy(static entry => entry.PeerEndpoint, StringComparer.Ordinal)
                .ThenBy(static entry => entry.ChannelName, StringComparer.Ordinal)
                .ToArray(),
            node.Subjects()
                .OrderBy(static entry => entry.Subject, StringComparer.Ordinal)
                .ThenBy(static entry => entry.Role)
                .ToArray());
    }
}