using System.Collections.Concurrent;
using Systems.Zlink;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Ops.Application.Ops;

/// <summary>
/// What the console knows about each node. Two sources feed it and neither is enough
/// alone (§8.1): the runtime events Ops can observe in its own process say whether a node
/// is registered and connected, and the node's own report says which zones it hosts, how
/// many players it holds and whether it is under maintenance.
/// </summary>
public sealed class NodeRegistry(TimeProvider timeProvider)
{
    public NodeRegistry() : this(TimeProvider.System)
    {
    }

    public static readonly TimeSpan ReportTtl =
        TimeSpan.FromMilliseconds(ZoneWorldSpec.NodeStatusReportTtlMs);

    private readonly ConcurrentDictionary<string, NodeState> _nodes = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, string> _nodeByRoutingId = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, string> _routingIdByNode = new(StringComparer.Ordinal);
    private IReadOnlySet<string> _liveRoutingIds = new HashSet<string>(StringComparer.Ordinal);

    public event Func<NodeView, CancellationToken, ValueTask>? Changed;

    public IReadOnlyList<NodeView> Snapshot() =>
        _nodes.Values
            .Select(state => state.View)
            .OrderBy(node => node.NodeId, StringComparer.Ordinal)
            .ToArray();

    public ValueTask ApplyRegistrationAsync(
        string nodeId,
        bool registered,
        CancellationToken cancellationToken) =>
        UpdateAsync(nodeId, state => state with
        {
            View = state.View with { Registered = registered }
        }, cancellationToken);

    public ValueTask ApplyConnectionAsync(
        string nodeId,
        bool connected,
        CancellationToken cancellationToken) =>
        UpdateAsync(nodeId, state => state with
        {
            TransportConnected = connected,
            View = state.View with { Connected = connected }
        }, cancellationToken);

    public string? NodeIdOf(string routingId) =>
        _nodeByRoutingId.TryGetValue(routingId, out var nodeId) ? nodeId : null;

    public async ValueTask ApplyLiveRoutingIdsAsync(
        IReadOnlySet<string> liveRoutingIds,
        CancellationToken cancellationToken)
    {
        _liveRoutingIds = liveRoutingIds;
        foreach (var nodeId in _nodes.Keys)
        {
            _routingIdByNode.TryGetValue(nodeId, out var routingId);
            await ApplyConnectionAsync(
                nodeId,
                routingId is not null && liveRoutingIds.Contains(routingId),
                cancellationToken);
        }
    }

    public async ValueTask<bool> ApplyReportAsync(
        ReportNodeStatusMsg report,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        var routingId = sourceNodeRid.ToString();
        var isLive = _liveRoutingIds.Contains(routingId);
        _routingIdByNode.TryGetValue(report.NodeId, out var previousRoutingId);
        var connectionCorrelated = isLive
            && (!_nodes.TryGetValue(report.NodeId, out var previousState)
                || !previousState.View.Connected
                || previousRoutingId is null
                || !string.Equals(previousRoutingId, routingId, StringComparison.Ordinal));
        if (_nodeByRoutingId.TryGetValue(routingId, out var previousNodeId)
            && previousNodeId != report.NodeId)
            _routingIdByNode.TryRemove(previousNodeId, out _);
        if (previousRoutingId is { } previous && previous != routingId)
            _nodeByRoutingId.TryRemove(previous, out _);
        _routingIdByNode[report.NodeId] = routingId;
        _nodeByRoutingId[routingId] = report.NodeId;
        await UpdateAsync(report.NodeId, state => state with
        {
            TransportConnected = isLive,
            LastReportTimestamp = timeProvider.GetTimestamp(),
            View = state.View with
            {
                Registered = true,
                Connected = isLive,
                Zones = report.Zones,
                PlayerCount = report.PlayerCount,
                Maintenance = report.Maintenance
            }
        }, cancellationToken);
        return connectionCorrelated;
    }

    /// <summary>
    /// Expires positive explicit reports after 15 seconds. A transport event never performs
    /// this transition: a crashed node cannot submit a false report, so TTL is the sole owner.
    /// </summary>
    public async ValueTask ExpireStaleReportsAsync(CancellationToken cancellationToken)
    {
        var now = timeProvider.GetTimestamp();
        foreach (var (nodeId, observed) in _nodes.ToArray())
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!observed.View.Registered
                || observed.LastReportTimestamp is not { } lastReport
                || timeProvider.GetElapsedTime(lastReport, now) < ReportTtl)
                continue;

            var expired = observed with
            {
                View = observed.View with { Registered = false }
            };
            if (!_nodes.TryUpdate(nodeId, expired, observed)) continue;
            await NotifyChangedAsync(expired.View, cancellationToken);
        }
    }

    private async ValueTask UpdateAsync(
        string nodeId,
        Func<NodeState, NodeState> change,
        CancellationToken cancellationToken)
    {
        var updated = _nodes.AddOrUpdate(
            nodeId,
            _ => change(Empty(nodeId)),
            (_, existing) => change(existing));

        await NotifyChangedAsync(updated.View, cancellationToken);
    }

    private async ValueTask NotifyChangedAsync(
        NodeView updated,
        CancellationToken cancellationToken)
    {
        if (Changed is not { } changed) return;
        foreach (var handler in changed.GetInvocationList()
                     .Cast<Func<NodeView, CancellationToken, ValueTask>>())
            await handler(updated, cancellationToken);
    }

    private static NodeState Empty(string nodeId) =>
        new(
            new NodeView(
                nodeId,
                Registered: false,
                Connected: false,
                Maintenance: false,
                Zones: [],
                PlayerCount: 0),
            TransportConnected: false,
            LastReportTimestamp: null);

    private sealed record NodeState(
        NodeView View,
        bool TransportConnected,
        long? LastReportTimestamp);
}
