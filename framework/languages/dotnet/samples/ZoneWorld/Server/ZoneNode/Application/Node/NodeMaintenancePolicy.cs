using System.Collections.Concurrent;
namespace ZoneWorld.Server.ZoneNode.Application.Node;

/// <summary>
/// Maintenance mode (§2.3). The node judging a move is the one the player is leaving,
/// so it has to know the target node's state too. Reading the store on every move
/// would be expensive, so each node caches every node's state: seeded from the store
/// at startup and refreshed by the maintenance fanout.
///
/// The cache is an optimisation. Authority sits with the target node, which re-judges
/// on arrival and rejects the relocation if it is under maintenance — so a stale cache
/// costs a rejected move, never an admitted one.
/// </summary>
public sealed class NodeMaintenancePolicy(string ownNodeId)
{
    private readonly ConcurrentDictionary<string, bool> _byNode = new(StringComparer.Ordinal);

    public string OwnNodeId { get; } = ownNodeId;

    /// <summary>The authoritative answer for this node. No cache is involved.</summary>
    public bool IsOwnNodeUnderMaintenance => _byNode.TryGetValue(OwnNodeId, out var enabled) && enabled;

    public bool IsUnderMaintenance(string nodeId) =>
        _byNode.TryGetValue(nodeId, out var enabled) && enabled;

    /// <summary>
    /// Maintenance blocks a new entry and a move from another logical node. A move between
    /// zones already hosted by the same node does not introduce new workload and remains
    /// allowed while that node is being drained.
    /// </summary>
    public bool RejectsArrival(string targetNodeId, string? sourceNodeId) =>
        IsUnderMaintenance(targetNodeId)
        && !string.Equals(targetNodeId, sourceNodeId, StringComparison.Ordinal);

    public void Apply(string nodeId, bool enabled) => _byNode[nodeId] = enabled;

    public IReadOnlyDictionary<string, bool> Snapshot() =>
        _byNode.ToDictionary(entry => entry.Key, entry => entry.Value, StringComparer.Ordinal);
}
