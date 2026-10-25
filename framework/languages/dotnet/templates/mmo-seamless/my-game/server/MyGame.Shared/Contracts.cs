using System.Text.Json;
using System.Text.Json.Serialization;

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
    public const long MaxSafeInteger = 9_007_199_254_740_991;
    public static int WorldWidth => ZoneWidth * ZoneCount;

    public static bool IsSafeInteger(ulong value) => value <= (ulong)MaxSafeInteger;

    public static string ZoneId(double x) => $"zone-{(int)Math.Floor(x / ZoneWidth) + 1}";

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
    string ZoneId,
    string NodeId,
    double X,
    long Sequence
);

public sealed record Position(double X);

public sealed record JoinWorldRequest(string PlayerId, double X);

public sealed record MoveRequest(double Delta, long Sequence);

public sealed record EnterZone(Position Position, long Sequence, int RemainingMs);

public sealed class PlayerLeftReasonConverter() : JsonStringEnumConverter<PlayerLeftReason>(
    JsonNamingPolicy.KebabCaseLower
)
{ }

[JsonConverter(typeof(PlayerLeftReasonConverter))]
public enum PlayerLeftReason
{
    LeftZone,
    Disconnected,
}

public sealed class BorderPresenceKindConverter() : JsonStringEnumConverter<BorderPresenceKind>(
    JsonNamingPolicy.KebabCaseLower
)
{ }

[JsonConverter(typeof(BorderPresenceKindConverter))]
public enum BorderPresenceKind
{
    Appeared,
    Moved,
    Left,
}

public sealed record PlayerAppeared(string PlayerId, Position Position);

public sealed record PlayerMoved(string PlayerId, Position Position, long Sequence);

public sealed record PlayerLeft(string PlayerId, PlayerLeftReason Reason);

public sealed record BorderPresence(
    string SourceZoneId,
    string PlayerId,
    Position Position,
    long Sequence,
    BorderPresenceKind Kind,
    PlayerLeftReason? Reason
);

public sealed record ActionRejected(string Reason, int RetryAfterMs);

public sealed record OpenDungeonRequest(string DungeonId);

public sealed record DungeonOpened(string DungeonId, string NodeId);

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string? ActorId, long? ObjectGeneration);

public sealed record NodeReady(string Id, string MeshEndpoint, string? StreamEndpoint);

public static class NodeProtocol
{
    // Marker declarations register themselves; commands remain compile-time constants.
    private static readonly List<(string Text, bool Prefix)> Markers = [];

    private static string Marker(string text, bool prefix = false)
    {
        Markers.Add((text, prefix));
        return text;
    }

    public static bool IsMarker(string line)
    {
        foreach (var marker in Markers)
            if (
                marker.Prefix
                    ? line.StartsWith(marker.Text, StringComparison.Ordinal)
                    : line == marker.Text
            )
                return true;
        return false;
    }

    public const int ReplacementX = 0;
    public const string VacateZone = "vacate-zone";
    public static readonly string ZoneVacated = Marker("ZONE_VACATED");
    public static readonly string ReadyPrefix = Marker("READY ", prefix: true);
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public static readonly string RelocationRequested = Marker("RELOCATION_REQUESTED");
    public static readonly string Relocated = Marker("RELOCATED");
    public static readonly string SafeToShutdown = Marker("SAFE_TO_SHUTDOWN");
}
