using RuntimeMonitoring.Server.Service.Support;

namespace RuntimeMonitoring.Server.Service.Handlers;

// Public RouteMesh status changes provide peer and ChannelName readiness
// without exposing descriptor generations, endpoints, or provider records.
internal sealed class MeshEventRecorder(
    EvidenceStore evidence,
    Zlink.Framework.Contracts.Configuration.IZLinkRouteMeshRuntime meshRuntime)
    : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var channel = ObserveAsync(RuntimeMonitoring.Shared.RuntimeMonitoringNames.Channel, stoppingToken);
        var spot = ObserveAsync(RuntimeMonitoring.Shared.RuntimeMonitoringNames.SpotChannel, stoppingToken);
        await Task.WhenAll(channel, spot).ConfigureAwait(false);
    }

    private async Task ObserveAsync(string meshName, CancellationToken cancellationToken)
    {
        var previousPeers = new HashSet<string>(StringComparer.Ordinal);
        var previousChannels = new Dictionary<string, (bool Ready, int Targets)>(
            StringComparer.Ordinal);
        await foreach (var status in meshRuntime
                           .ObserveAsync(meshName, cancellationToken)
                           .ConfigureAwait(false))
        {
            var currentPeers = status.Status.Peers
                .Where(static peer =>
                    peer.State == Zlink.Framework.Contracts.Configuration.ZLinkPeerState.Ready)
                .Select(static peer => peer.NodeRid.ToString())
                .ToHashSet(StringComparer.Ordinal);
            foreach (var peer in currentPeers.Except(previousPeers, StringComparer.Ordinal))
            {
                RecordPeer(meshName, "ConnectionReady", peer, status.Status);
            }
            foreach (var peer in previousPeers.Except(currentPeers, StringComparer.Ordinal))
            {
                RecordPeer(meshName, "Disconnected", peer, status.Status);
            }

            foreach (var channel in status.Status.Channels)
            {
                var current = (channel.IsReady, channel.ReadyTargetCount);
                if (!previousChannels.TryGetValue(channel.ChannelName, out var previous)
                    || previous != current)
                {
                    evidence.Add(
                        $"monitor-mesh|source={meshName}"
                        + "|identifier=zlink.runtime.mesh_node.channel_changed"
                        + $"|kind=ReadinessChanged|channel={channel.ChannelName}"
                        + $"|ready={channel.IsReady}|targets={channel.ReadyTargetCount}"
                        + $"|sequence={status.Status.Sequence}|state={status.Status.State}");
                    previousChannels[channel.ChannelName] = current;
                }
            }
            previousPeers = currentPeers;
        }
    }

    private void RecordPeer(
        string meshName,
        string kind,
        string peer,
        Zlink.Framework.Contracts.Configuration.ZLinkRouteMeshStatus status)
    {
        evidence.Add(
            $"monitor-mesh|source={meshName}|identifier=zlink.runtime.mesh_node.peer_changed|kind={kind}"
            + $"|routing={peer}|sequence={status.Sequence}"
            + $"|state={status.State}");
    }
}
