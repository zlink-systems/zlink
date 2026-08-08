using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// The framework-owned subject view for a MeshNode: Core surfaces no subject
// table, but every spot subscription passes through the spot seam, so the
// node tracks (spot, topic) pairs for the monitoring snapshot (spec 50).
internal sealed class ZLinkSpotSubscriptionTracker
{
    private readonly ConcurrentDictionary<ZLinkSpotId, ConcurrentDictionary<SubscriptionKey, byte>> _targets = new();

    public void Add(string spotId, string channelName, string topic)
    {
        var key = ZLinkSpotId.FromBoundary(spotId, nameof(spotId));
        _targets.GetOrAdd(key, static _ => new ConcurrentDictionary<SubscriptionKey, byte>())
            .TryAdd(new SubscriptionKey(channelName, topic), 0);
    }

    public void RemoveSpot(string spotId)
    {
        _targets.TryRemove(
            ZLinkSpotId.FromBoundary(spotId, nameof(spotId)),
            out _);
    }

    public IReadOnlyList<ZLinkSpotNodeSubjectEntry> Snapshot()
    {
        var entries = new List<ZLinkSpotNodeSubjectEntry>();
        foreach (var (_, targets) in _targets)
        foreach (var target in targets.Keys)
            entries.Add(new ZLinkSpotNodeSubjectEntry(
                ZLinkSpotRole.Sub,
                $"{target.ChannelName}:{target.Topic}",
                ZLinkSubjectKind.Topic,
                ReadyPeerCount: 0,
                ActivePeerCount: 0,
                LastChangedMs: 0));
        return entries;
    }

    private readonly record struct SubscriptionKey(string ChannelName, string Topic);
}
