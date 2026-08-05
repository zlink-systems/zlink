using System.Collections.Concurrent;

namespace ZoneWorld.Server.ZoneNode.Application.Zone;

/// <summary>
/// How many players this node currently holds, counted per zone. The node needs one
/// number across all of its zone spots to answer diagnostics and the status report
/// (§8.1), but each zone spot only knows its own residents, so the count is kept here.
/// Zone spot callbacks are serial within a spot, not across spots, hence the
/// concurrent map.
/// </summary>
public sealed class NodePlayerCensus
{
    private readonly ConcurrentDictionary<string, int> _byZone = new(StringComparer.Ordinal);

    public void Record(string zoneId, int playerCount) => _byZone[zoneId] = playerCount;

    public int TotalPlayers => _byZone.Values.Sum();

    public IReadOnlyList<string> ZoneIds =>
        _byZone.Keys.OrderBy(value => value, StringComparer.Ordinal).ToArray();
}
