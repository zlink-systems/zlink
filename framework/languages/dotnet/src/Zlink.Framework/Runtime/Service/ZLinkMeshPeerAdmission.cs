using Systems.Zlink;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal readonly record struct ZLinkMeshPeerExpectation(
    string Endpoint,
    string SecurityIdentity,
    ulong LifecycleGeneration
);

/// <summary>
/// The last observed Core-selected route of each RID (Core ROUTER §10.1). The
/// mesh node receive loop is the socket's single route observer; this type
/// only records what the snapshot reported and never selects a route.
/// </summary>
internal sealed class ZLinkMeshSelectedRoutes
{
    private Dictionary<RoutingId, ulong> _routes = [];

    internal IEnumerable<RoutingId> RoutingIds => _routes.Keys;

    /// <summary>
    /// The observed route generation of the RID, or 0 when the RID has no
    /// selected route. Core never reports 0 for a selected route (Core ROUTER
    /// §10.1), so the value is compared only for equality.
    /// </summary>
    internal ulong GenerationOf(RoutingId routingId) =>
        _routes.TryGetValue(routingId, out var generation) ? generation : 0;

    /// <summary>
    /// Replaces the observation and returns every previously observed route
    /// (RID and generation) that is missing from the snapshot or was replaced
    /// by a new generation.
    /// </summary>
    internal IReadOnlyList<RouterRoute> Apply(IReadOnlyList<RouterRoute> snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        var next = new Dictionary<RoutingId, ulong>(snapshot.Count);
        foreach (var route in snapshot)
            next[route.RoutingId] = route.RouteGeneration;
        var ended = new List<RouterRoute>();
        foreach (var (routingId, generation) in _routes)
            if (!next.TryGetValue(routingId, out var current) || current != generation)
                ended.Add(new RouterRoute(routingId, generation));
        _routes = next;
        return ended;
    }

    internal void Clear() => _routes.Clear();
}
