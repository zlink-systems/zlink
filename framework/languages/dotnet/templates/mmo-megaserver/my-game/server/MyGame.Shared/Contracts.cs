using System.Text.Json.Serialization;

namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-world";
    public const string RegionType = "Region";
    public const string PlayerType = "Player";
    public const string DungeonType = "Dungeon";
    public const string SessionStream = "players";
    public const int DefaultCapacity = 2;
    public const int DefaultRegionLimit = 2;
    public const int MaxReplicasPerRegion = 2;
    public const int MaxStep = 10;
    public const int RegionMinX = 0;
    public const int RegionMaxX = 100;
    public const double MaxSpeed = MaxStep * 5.0;
    public const long MaxSafeInteger = 9_007_199_254_740_991;

    public static IReadOnlyList<string> AllowedRegions { get; } = Array.AsReadOnly(
        new[] { "forest", "desert" }
    );

    public static string RegionId(string region, int index) => $"{region}-{index}";

    public static bool IsAllowedRegion(string? region) =>
        region is not null && AllowedRegions.Contains(region, StringComparer.Ordinal);
}

public static class Rejection
{
    public const string RegionFull = "region-full";
    public const string PacketNotAllowed = "packet-not-allowed";
    public const string NonFinite = "non-finite";
    public const string StepTooLarge = "step-too-large";
    public const string OutsideRegion = "outside-region";
    public const string MoveBudget = "move-budget";
    public const string StaleSequence = "stale-sequence";
    public const string RegionUnknown = "region-unknown";
    public const string CapacityExhausted = "capacity-exhausted";
    public const string IntegerOutOfRange = "integer-out-of-range";
}

public sealed record EnterRegionRequest(string PlayerId, string Region);

public sealed record JoinRegion(string Region, int Index);

public sealed record RegionFull(string Reason, string Region, int Index);

public sealed record RegionUnavailable(string Region, string Reason);

public sealed record MoveRequest(double Delta, long Sequence);

public sealed record ActionRejected(string Reason, int RetryAfterMs);

public sealed record PlayerPosition(string PlayerId, double X, long Sequence);

public sealed record RegionView(string RegionId, string NodeId, PlayerPosition[] Players);

public sealed record OpenDungeonRequest(string DungeonId);

public sealed record DungeonOpened(string DungeonId, string NodeId);

public sealed record DungeonInfoRequest;

public sealed record ViewRequest;

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);

public sealed record NodeReady(
    [property: JsonPropertyName("id")] string Id,
    [property: JsonPropertyName("meshEndpoint")] string MeshEndpoint,
    [property: JsonPropertyName("streamEndpoint")] string? StreamEndpoint
);

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

    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeObservation = MessageFollow + TimeSpan.FromSeconds(30);
    public static readonly string ReadyPrefix = Marker("READY ", prefix: true);
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public static readonly string Stopped = Marker("STOPPED");
    public static readonly string RelocationRequested = Marker("RELOCATION_REQUESTED");
    public static readonly string Relocated = Marker("RELOCATED");
    public static readonly string SafeToShutdown = Marker("SAFE_TO_SHUTDOWN");
}
