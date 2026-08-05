using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;

/// <summary>
/// The coordinate system and the zone split (§2). Pure rules: no ZLink type appears
/// here, so the same judgements can be unit tested and mirrored by the browser client.
/// </summary>
public static class World
{
    public static bool InRange(int x, int y) =>
        x >= 0 && x < ZoneWorldSpec.WorldSize && y >= 0 && y < ZoneWorldSpec.WorldSize;

    /// <summary>
    /// Zones that share an edge. The diagonal pair is not adjacent, which is why a
    /// move may not cross both boundaries at once (§2.2 DiagonalCrossing).
    /// </summary>
    public static IReadOnlyList<string> AdjacentZones(string zoneId) => zoneId switch
    {
        ZoneIds.NorthWest => [ZoneIds.NorthEast, ZoneIds.SouthWest],
        ZoneIds.NorthEast => [ZoneIds.NorthWest, ZoneIds.SouthEast],
        ZoneIds.SouthWest => [ZoneIds.NorthWest, ZoneIds.SouthEast],
        ZoneIds.SouthEast => [ZoneIds.NorthEast, ZoneIds.SouthWest],
        _ => throw new ArgumentOutOfRangeException(nameof(zoneId), zoneId, "Unknown zone.")
    };

    /// <summary>
    /// Whether a player standing at (x, y) inside <paramref name="fromZoneId"/> is close
    /// enough to the shared edge to be visible from <paramref name="toZoneId"/> (§4.1).
    /// The band is measured on the axis the two zones share an edge across.
    /// </summary>
    public static bool InBorderBand(int x, int y, string fromZoneId, string toZoneId)
    {
        if (ZoneWorldSpec.ZoneOf(x, y) != fromZoneId) return false;

        var split = ZoneWorldSpec.ZoneSplit;
        var band = ZoneWorldSpec.BorderBand;
        var crossesX = IsWest(fromZoneId) != IsWest(toZoneId);
        var crossesY = IsNorth(fromZoneId) != IsNorth(toZoneId);

        // Diagonal zones share no edge, so they have no band.
        if (crossesX == crossesY) return false;

        var distance = crossesX
            ? Math.Abs((IsWest(fromZoneId) ? split - 1 - x : x - split))
            : Math.Abs((IsNorth(fromZoneId) ? split - 1 - y : y - split));

        return distance < band;
    }

    public static bool IsWest(string zoneId) =>
        zoneId is ZoneIds.NorthWest or ZoneIds.SouthWest;

    public static bool IsNorth(string zoneId) =>
        zoneId is ZoneIds.NorthWest or ZoneIds.NorthEast;
}

public readonly record struct PlayerPosition(int X, int Y)
{
    public string ZoneId => ZoneWorldSpec.ZoneOf(X, Y);
}
