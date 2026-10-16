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

    public static string RegionId(string region, int index) => $"{region}-{index}";
}

public static class Rejection
{
    public const string RegionFull = "region-full";
}

public sealed record EnterRegionRequest(string PlayerId, string Region);

public sealed record JoinRegion(string Region, int Index);

public sealed record RegionFull(string Reason, string Region, int Index);

public sealed record MoveRequest(int X, long Sequence);

public sealed record PlayerPosition(string PlayerId, int X);

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
    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeObservation = MessageFollow + TimeSpan.FromSeconds(30);
    public const string ReadyPrefix = "READY ";
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public const string Stopped = "STOPPED";
    public const string RelocationRequested = "RELOCATION_REQUESTED";
    public const string Relocated = "RELOCATED";
    public const string SafeToShutdown = "SAFE_TO_SHUTDOWN";
}
