namespace MyGame.Shared;

public static class GameNames
{
    public const string ZoneType = "Zone";
    public const string PlayerType = "Player";
    public const string SessionStream = "players";
    public const string PlayerKeyPrefix = "player:";
    public const string RelocationKeyPrefix = "relocation:";
    private const string ZonePrefix = "field@";
    public const int FirstChannel = 1;
    public const int UpdateChannel = 3;

    public static string Mesh(string worldId) => "my-game-world-" + worldId.ToLowerInvariant();

    public static string ZoneId(int channel)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(channel, FirstChannel);
        return ZonePrefix + channel.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }

    public static int Channel(string spotId) =>
        int.Parse(spotId[ZonePrefix.Length..], System.Globalization.CultureInfo.InvariantCulture);
}

public sealed record EnterWorldRequest(string PlayerId);

public sealed record ChangeChannelRequest(int Channel);

public sealed record EarnRequest(long Amount);

public sealed record ViewRequest;

public sealed record PlayerView(string WorldId, int Channel, string NodeId, long Gold);

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);

public sealed record WorldInfo(string WorldId, string Name, string SessionEndpoint);

public sealed record NodeReady(
    string Id,
    string MeshEndpoint,
    string? StreamEndpoint = null,
    string? HttpEndpoint = null
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
    public static readonly TimeSpan StartupDeadline = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeDeadline = MessageFollow + StartupDeadline;
    public static readonly string ReadyPrefix = Marker("READY ", prefix: true);
    public const string Drain = "drain";
    public const string CreateUpdateZone = "create-update-zone";
    public static readonly string UpdateZoneCreated = Marker("UPDATE_ZONE_CREATED");
    public const string Shutdown = "shutdown";
    public static readonly string Stopped = Marker("STOPPED");
    public static readonly string RelocationRequested = Marker("RELOCATION_REQUESTED");
    public static readonly string Relocated = Marker("RELOCATED");
    public static readonly string SafeToShutdown = Marker("SAFE_TO_SHUTDOWN");
}
