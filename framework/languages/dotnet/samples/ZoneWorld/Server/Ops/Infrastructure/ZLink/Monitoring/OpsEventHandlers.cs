using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.Ops.Application.Ops;
using ZoneWorld.Server.Ops.Infrastructure.ZLink.Sessions;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Ops.Infrastructure.ZLink.Monitoring;

/// <summary>
/// Whether a node is registered (§8.1). This is a change notification, not a question Ops
/// can ask: a node that has shut down is not there to answer, so the console learns about
/// it from the location runtime rather than by polling.
/// </summary>
/// <summary>
/// Whether a node's connection is up (§8.1). The node reports over the channel using its own
/// routing id, so a socket event on that channel names the node it came from.
/// </summary>
internal sealed class SocketEventHandler(
    NodeRegistry nodes,
    IZLinkRouteMeshRuntime runtime,
    ILogger<SocketEventHandler> logger)
    : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var previous = new HashSet<string>(StringComparer.Ordinal);
        await foreach (var status in runtime
                           .ObserveAsync(ZoneWorldNames.MeshName, stoppingToken)
                           .ConfigureAwait(false))
        {
            var current = status.Status.Peers
                .Where(static peer => peer.State == ZLinkPeerState.Ready)
                .Select(static peer => peer.NodeRid.ToString())
                .ToHashSet(StringComparer.Ordinal);
            await nodes.ApplyLiveRoutingIdsAsync(current, stoppingToken);
            foreach (var rid in current.Except(previous))
                await ApplyAsync(rid, true, stoppingToken);
            foreach (var rid in previous.Except(current))
                await ApplyAsync(rid, false, stoppingToken);
            previous = current;
        }
    }

    private async Task ApplyAsync(
        string rid,
        bool connected,
        CancellationToken cancellationToken)
    {
        var nodeId = nodes.NodeIdOf(rid);
        if (nodeId is null)
            return;
        await nodes.ApplyConnectionAsync(nodeId, connected, cancellationToken);
        logger.LogInformation(
            "node connection observed. node={NodeId}, connected={Connected}",
            nodeId,
            connected);
    }
}

/// <summary>Pushes node state to the consoles as it changes. The console never polls
/// (§10.2), so every change has to leave through here.</summary>
internal sealed class NodeStatusBroadcaster(NodeRegistry nodes, OpsConsoleRegistry consoles) : IHostedService
{
    public Task StartAsync(CancellationToken cancellationToken)
    {
        nodes.Changed += Push;
        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        nodes.Changed -= Push;
        return Task.CompletedTask;
    }

    private async ValueTask Push(NodeView node, CancellationToken cancellationToken) =>
        await consoles.BroadcastAsync(new NodeStatusNotify(
            node.NodeId,
            node.Registered,
            node.Connected,
            node.Maintenance,
            node.Zones,
            node.PlayerCount), cancellationToken);
}
