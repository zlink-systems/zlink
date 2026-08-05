using ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Application.Zone;

public sealed record BotRoute(string PlayerId, int X, int Y, int DirX, int DirY, string ZoneId);

/// <summary>
/// The bots (§2.7). Their paths are fixed rather than random: five language servers have
/// to produce the same world, and a random walk would make the self-check unstable.
/// Four of them patrol across the X boundary, so a cross-node actor relocation keeps
/// happening even with no client connected.
/// </summary>
public static class BotPatrolPolicy
{
    public static readonly IReadOnlyList<BotRoute> Routes =
    [
        new(BotIds.NorthWestX, 10, 15, +1, 0, ZoneIds.NorthWest),
        new(BotIds.NorthWestY, 15, 10, 0, +1, ZoneIds.NorthWest),
        new(BotIds.NorthEastX, 90, 15, -1, 0, ZoneIds.NorthEast),
        new(BotIds.NorthEastY, 85, 10, 0, +1, ZoneIds.NorthEast),
        new(BotIds.SouthWestX, 10, 85, +1, 0, ZoneIds.SouthWest),
        new(BotIds.SouthWestY, 15, 90, 0, -1, ZoneIds.SouthWest),
        new(BotIds.SouthEastX, 90, 85, -1, 0, ZoneIds.SouthEast),
        new(BotIds.SouthEastY, 85, 90, 0, -1, ZoneIds.SouthEast)
    ];

    public static IReadOnlyList<BotRoute> RoutesOf(string zoneId) =>
        Routes.Where(route => route.ZoneId == zoneId).ToArray();

    public static bool IsBot(string playerId) =>
        Routes.Any(route => route.PlayerId == playerId);

    public static PlayerPosition NextStep(PlayerPosition from, int dirX, int dirY) =>
        new(from.X + dirX * ZoneWorldSpec.BotStep, from.Y + dirY * ZoneWorldSpec.BotStep);
}
