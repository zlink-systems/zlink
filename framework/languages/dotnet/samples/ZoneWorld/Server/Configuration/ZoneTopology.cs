using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Configuration;

/// <summary>The world map. It contains business ZoneIds only; runtime placement belongs
/// to the framework location directory.</summary>
public static class ZoneTopology
{
    public static IReadOnlyList<string> Zones { get; } =
    [
        ZoneIds.NorthWest,
        ZoneIds.NorthEast,
        ZoneIds.SouthWest,
        ZoneIds.SouthEast
    ];

    /// <summary>The zone a new player spawns into (§2, spawn coordinate is fixed).</summary>
    public static string SpawnZone => ZoneWorldSpec.ZoneOf(ZoneWorldSpec.SpawnX, ZoneWorldSpec.SpawnY);

}
