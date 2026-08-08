using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Service;

/// <summary>
/// Owns the channel target plans for one managed mesh node.
/// </summary>
/// <remarks>
/// The mesh node calls this type while holding its topology gate. Keeping the
/// plan lifecycle here prevents selection state from being mixed with peer
/// admission and message submission state.
/// </remarks>
internal sealed class ZLinkMeshChannelSelection
{
    private readonly Dictionary<ZLinkChannelName,
        ZLinkWeightedSelectionPlan<ZLinkMeshChannelTarget, string>> _plans = [];
    private readonly HashSet<ZLinkChannelName> _declaredChannels = [];

    internal bool TrySelect(string channelName, out RoutingId targetRid)
    {
        var channel = Channel(channelName);
        if (!_plans.TryGetValue(channel, out var plan)
            || plan.Count == 0)
        {
            targetRid = default;
            return false;
        }

        targetRid = plan.Select()!.RoutingId;
        return true;
    }

    internal IReadOnlyList<ZLinkMeshChannelTarget> Candidates(
        string channelName) =>
        _plans.TryGetValue(Channel(channelName), out var plan)
            ? plan.Candidates
            : Array.Empty<ZLinkMeshChannelTarget>();

    internal bool IsDeclared(string channelName) =>
        _declaredChannels.Contains(Channel(channelName));

    internal void Rebuild(
        IEnumerable<string> channelNames,
        Func<string, IReadOnlyList<ZLinkMeshChannelTarget>> targetFactory)
    {
        ArgumentNullException.ThrowIfNull(channelNames);
        ArgumentNullException.ThrowIfNull(targetFactory);

        var names = channelNames
            .Select(static channelName =>
                ZLinkChannelName.FromBoundary(channelName, nameof(channelNames)))
            .ToHashSet();
        _declaredChannels.Clear();
        _declaredChannels.UnionWith(names);

        foreach (var stale in _plans.Keys
                     .Where(key => !names.Contains(key))
                     .ToArray())
            _plans.Remove(stale);

        foreach (var channelName in names)
        {
            var retainedCurrents = _plans.TryGetValue(
                    channelName,
                    out var previousPlan)
                ? previousPlan.CaptureCurrents()
                : null;
            var targets = targetFactory(channelName.Value);
            _plans[channelName] =
                new ZLinkWeightedSelectionPlan<ZLinkMeshChannelTarget, string>(
                    targets,
                    static target => target.Weight,
                    static target => target.SelectionKey,
                    retainedCurrents,
                    StringComparer.Ordinal,
                    StringComparer.Ordinal);
        }
    }

    private static ZLinkChannelName Channel(string channelName) =>
        ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
}

internal sealed class ZLinkMeshChannelTarget
{
    internal ZLinkMeshChannelTarget(RoutingId routingId, int weight)
    {
        RoutingId = routingId;
        Weight = weight;
        SelectionKey = routingId.ToHex();
    }

    internal RoutingId RoutingId { get; }
    internal int Weight { get; }
    internal string SelectionKey { get; }
}
