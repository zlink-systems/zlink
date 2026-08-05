using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;

/// <summary>
/// One zone's view of the world: the players it owns, plus the latest border snapshot
/// each adjacent zone published. The player actor owns the authoritative coordinate;
/// what lives here is a copy kept for rendering and border sync (§2.1).
/// </summary>
public sealed class ZoneState(string zoneId)
{
    private readonly Dictionary<string, ResidentPlayer> _residents = new(StringComparer.Ordinal);
    private readonly Dictionary<string, BorderSnapshot> _adjacent = new(StringComparer.Ordinal);

    public string ZoneId { get; } = zoneId;

    public long Tick { get; private set; }

    public int PlayerCount => _residents.Count;

    public IReadOnlyCollection<string> ResidentIds => _residents.Keys;

    public bool TryGetResident(string playerId, out ResidentPlayer resident) =>
        _residents.TryGetValue(playerId, out resident!);

    public void Enter(string playerId, int x, int y, bool isBot) =>
        _residents[playerId] = new ResidentPlayer(playerId, new PlayerPosition(x, y), isBot);

    public void UpdatePosition(string playerId, int x, int y, bool isBot)
    {
        if (_residents.TryGetValue(playerId, out var resident))
            _residents[playerId] = resident with
            {
                Position = new PlayerPosition(x, y),
                IsBot = isBot
            };
    }

    public void Leave(string playerId) => _residents.Remove(playerId);

    /// <summary>
    /// Replaces the snapshot for one adjacent zone wholesale. Snapshots are not merged:
    /// an empty player list is a valid snapshot and means "nobody is in the band" (§2.4).
    /// An out-of-order snapshot is dropped rather than applied.
    /// </summary>
    public void ApplyBorderSnapshot(string fromZoneId, long tick, IReadOnlyList<PlayerView> players)
    {
        if (_adjacent.TryGetValue(fromZoneId, out var current) && tick <= current.Tick) return;

        _adjacent[fromZoneId] = new BorderSnapshot(tick, Tick, players);
    }

    public long NextTick() => ++Tick;

    /// <summary>Drops adjacent snapshots that stopped arriving, so a stopped node's players
    /// do not linger on screen (§2.4).</summary>
    public void ExpireStaleSnapshots()
    {
        var expired = _adjacent
            .Where(entry => Tick - entry.Value.ReceivedAtTick >= ZoneWorldSpec.BorderSnapshotExpiryTicks)
            .Select(entry => entry.Key)
            .ToArray();

        foreach (var zoneId in expired) _adjacent.Remove(zoneId);
    }

    /// <summary>
    /// The player list pushed to clients: this zone's residents merged with the players
    /// the adjacent zones reported in their border bands. On a duplicate PlayerId this
    /// zone's own value wins, and the result is ordered by PlayerId as UTF-8 bytes so
    /// every language produces the same list (§2.4).
    /// </summary>
    public IReadOnlyList<PlayerView> VisiblePlayers()
    {
        var merged = new Dictionary<string, PlayerView>(StringComparer.Ordinal);

        foreach (var snapshot in _adjacent.Values)
        foreach (var player in snapshot.Players)
            merged[player.PlayerId] = player;

        foreach (var resident in _residents.Values)
            merged[resident.PlayerId] = resident.ToView(ZoneId);

        return merged.Values
            .OrderBy(player => player.PlayerId, StringComparer.Ordinal)
            .ToArray();
    }

    /// <summary>The residents standing close enough to the shared edge to be visible from
    /// <paramref name="toZoneId"/> (§4.1).</summary>
    public IReadOnlyList<PlayerView> BorderBandFor(string toZoneId) =>
        _residents.Values
            .Where(resident => World.InBorderBand(
                resident.Position.X, resident.Position.Y, ZoneId, toZoneId))
            .Select(resident => resident.ToView(ZoneId))
            .OrderBy(player => player.PlayerId, StringComparer.Ordinal)
            .ToArray();

    private sealed record BorderSnapshot(long Tick, long ReceivedAtTick, IReadOnlyList<PlayerView> Players);
}

public readonly record struct ResidentPlayer(string PlayerId, PlayerPosition Position, bool IsBot)
{
    public PlayerView ToView(string zoneId) =>
        new(PlayerId, Position.X, Position.Y, zoneId, IsBot);
}
