using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.Ops.Ports;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Ops.Application.Ops;

/// <summary>
/// Publishes a world announcement (§8.2) and owns its identity. The operations port hides the
/// fanout topic, so the use case has no node list and adding a node changes nothing here.
/// </summary>
public sealed class AnnouncementService(IWorldOperationsPort operations)
{
    private int _sequence;

    public async ValueTask<string> PublishAsync(string text, CancellationToken cancellationToken)
    {
        var announcementId = $"ann-{Interlocked.Increment(ref _sequence):D4}";
        await operations.PublishAnnouncementAsync(announcementId, text, cancellationToken);
        return announcementId;
    }
}

/// <summary>
/// Switches one node's maintenance mode (§8.4). The desired state is written first so it
/// survives a restart of the target node — the call itself may not even reach a node that
/// is currently down. The response can report that the live application step was unavailable,
/// but the desired state itself is not lost.
/// </summary>
public sealed class MaintenanceService(
    IMaintenanceStorePort store,
    IWorldOperationsPort operations,
    NodeRegistry nodes)
{
    public async ValueTask<SetMaintenanceRes> SetAsync(
        string nodeId,
        bool enabled,
        CancellationToken cancellationToken)
    {
        await store.WriteAsync(nodeId, enabled, cancellationToken);
        await operations.PublishMaintenanceChangeAsync(nodeId, enabled, cancellationToken);

        // The application addresses a logical node ID only. Fanout applies the
        // desired state; no sample code translates it to a transport NodeRid.
        var node = nodes.Snapshot()
            .SingleOrDefault(candidate => candidate.NodeId == nodeId);
        return node is not { Registered: true, Connected: true }
            ? new SetMaintenanceRes(
                nodeId,
                enabled,
                [],
                ZoneWorldErrors.NodeUnavailable)
            : new SetMaintenanceRes(node.NodeId, enabled, node.Zones);
    }
}

/// <summary>
/// Reads the latest status report already observed by Ops. Diagnostics do not turn a
/// transport NodeRid into an application routing address.
/// </summary>
public sealed class NodeDiagnosticsService(NodeRegistry nodes)
{
    public ValueTask<NodeDiagnosticsRes> GetAsync(
        string nodeId,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var node = nodes.Snapshot()
            .SingleOrDefault(candidate => candidate.NodeId == nodeId);
        return ValueTask.FromResult(
            node is not { Registered: true, Connected: true }
                ? new NodeDiagnosticsRes(
                nodeId,
                [],
                PlayerCount: 0,
                Maintenance: false,
                ZoneWorldErrors.NodeUnavailable)
                : new NodeDiagnosticsRes(
                    node.NodeId,
                    node.Zones,
                    node.PlayerCount,
                    node.Maintenance));
    }
}
