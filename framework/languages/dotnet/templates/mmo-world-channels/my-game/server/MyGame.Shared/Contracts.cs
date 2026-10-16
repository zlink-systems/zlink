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
    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan StartupDeadline = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeDeadline = MessageFollow + StartupDeadline;
    public const string ReadyPrefix = "READY ";
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public const string Stopped = "STOPPED";
    public const string RelocationRequested = "RELOCATION_REQUESTED";
    public const string Relocated = "RELOCATED";
    public const string SafeToShutdown = "SAFE_TO_SHUTDOWN";
}
