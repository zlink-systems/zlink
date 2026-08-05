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

    /// <summary>
    /// Returns the zone objects assigned to a configured ZoneNode. The Location Store still
    /// chooses the current runtime owner; this table only prevents two application processes
    /// from racing to create every Zone Spot during startup.
    /// </summary>
    public static IReadOnlyList<string> ZonesOf(string nodeId) => nodeId switch
    {
        NodeIds.West => [ZoneIds.NorthWest, ZoneIds.SouthWest],
        NodeIds.East => [ZoneIds.NorthEast, ZoneIds.SouthEast],
        _ => []
    };

    /// <summary>
    /// Returns the logical node that owns a zone in the sample topology. A replacement
    /// process keeps this NodeId, so the value is suitable for the maintenance admission
    /// payload while the Framework location directory continues to own runtime routing.
    /// </summary>
    public static string NodeOf(string zoneId) => zoneId switch
    {
        ZoneIds.NorthWest or ZoneIds.SouthWest => NodeIds.West,
        ZoneIds.NorthEast or ZoneIds.SouthEast => NodeIds.East,
        _ => throw new ArgumentOutOfRangeException(nameof(zoneId), zoneId, "Unknown ZoneId.")
    };

    /// <summary>The zone a new player spawns into (§2, spawn coordinate is fixed).</summary>
    public static string SpawnZone => ZoneWorldSpec.ZoneOf(ZoneWorldSpec.SpawnX, ZoneWorldSpec.SpawnY);

}
