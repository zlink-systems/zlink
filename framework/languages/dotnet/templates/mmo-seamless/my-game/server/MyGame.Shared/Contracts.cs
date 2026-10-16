namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-world";
    public const string ZoneType = "Zone";
    public const string PlayerType = "Player";
    public const string DungeonType = "Dungeon";
    public const string BorderChannel = "border";
    public const string SessionStream = "players";
    public const int ZoneWidth = 100;
    public const int ZoneCount = 6;
    public const int BorderRadius = 10;
    public const int TickPeriodMilliseconds = 100;

    public static string ZoneId(int x)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(x);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(x, ZoneWidth * ZoneCount - 1);
        return $"zone-{x / ZoneWidth + 1}";
    }

    public static IEnumerable<string> AdjacentZones(string zoneId)
    {
        var number = int.Parse(zoneId.AsSpan("zone-".Length));
        if (number > 1)
            yield return $"zone-{number - 1}";
        if (number < ZoneCount)
            yield return $"zone-{number + 1}";
    }

    public static string BorderTopic(string source, string target) => $"{source}:{target}";
}

public sealed record WorldState(
    string ActorId,
    ulong ObjectGeneration,
    string ZoneId,
    string NodeId,
    int X,
    int Sequence
);

public sealed record JoinWorldRequest(string PlayerId, int X);

public sealed record MoveRequest(int X, int Sequence);

public sealed record EnterZone(int X, int Sequence);

public sealed record BorderPresence(string SourceZoneId, string ActorId, int X, int Sequence);

public sealed record BorderVisible(
    string ObserverZoneId,
    string SourceZoneId,
    string ActorId,
    int X,
    int Sequence
);

public sealed record OpenDungeonRequest(string DungeonId);

public sealed record DungeonOpened(string DungeonId, string NodeId);

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId);

public sealed record NodeReady(string Id, string MeshEndpoint, string? StreamEndpoint);

public static class NodeProtocol
{
    public const int ReplacementX = 0;
    public const string VacateZone = "vacate-zone";
    public const string ZoneVacated = "ZONE_VACATED";
    public const string ReadyPrefix = "READY ";
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public const string RelocationRequested = "RELOCATION_REQUESTED";
    public const string Relocated = "RELOCATED";
    public const string SafeToShutdown = "SAFE_TO_SHUTDOWN";
}
