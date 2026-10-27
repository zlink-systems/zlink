using System.Text.Json.Serialization;

namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-world";
    public const string TownType = "Town";
    public const string PlayerType = "Player";
    public const string DungeonType = "Dungeon";
    public const string SessionStream = "players";
    public const int MaximumPartyMembers = 4;
    public const int ChannelCount = 2;
    public const long MaximumInteger = 9_007_199_254_740_991;
    public const long FinalStage = 3;

    public static string TownId(long channel)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(channel, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(channel, ChannelCount);
        return $"town-{channel}";
    }

    public static string DungeonId(string runId) => $"dungeon-{runId}";
}

public static class Rejection
{
    public const string JoinRejected = "join-rejected";
    public const string InvalidParty = "invalid-party";
    public const string NotLeader = "not-leader";
    public const string RunMismatch = "run-mismatch";
    public const string RunInProgress = "run-in-progress";
    public const string RunNotActive = "run-not-active";
    public const string IntegerOutOfRange = "integer-out-of-range";
    public const string InvalidChannel = "invalid-channel";
    public const string PacketNotAllowed = "packet-not-allowed";
}

public sealed record JoinTownRequest(string PlayerId, long Channel);

public sealed record ChangeChannelRequest(long Channel);

public sealed record CreatePartyRequest;

public sealed record JoinPartyRequest(string PartyId);

public sealed record EnterDungeonRequest;

public sealed record AdvanceRequest;

public sealed record ViewRequest;

public sealed record PartyView(string PartyId, string[] Members, RunPhase Phase, string? RunId);

public sealed record TownView(string TownId, string NodeId, string[] Players, PartyView[] Parties);

public sealed record DungeonView(
    string DungeonId,
    string NodeId,
    string PartyId,
    string[] Members,
    long Stage,
    string RunId,
    RunPhase Phase
);

public sealed record PrepareDungeon(
    string RunId,
    string PartyId,
    string OwnerSpotId,
    string[] Members
);

public sealed record DungeonPrepared(string DungeonId, string RunId, string? Reason = null);

public sealed record JoinDungeon(string DungeonId, string RunId);

public sealed record ActionRejected(string Reason, int RetryAfterMs = 0);

[JsonConverter(typeof(JsonStringEnumConverter<RunPhase>))]
public enum RunPhase { Forming, Preparing, Entering, Active, Completed }

public sealed record RunStarted(string RunId);
public sealed record RunCompleted(string RunId);

public sealed record PartyViewRequest(string PlayerId);

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);

public sealed record NodeReady(string Id, string MeshEndpoint, string? StreamEndpoint);

public static class LifecycleLimits
{
    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan Relocation = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan Safe = MessageFollow + Relocation;
}

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

    public static readonly string ReadyPrefix = Marker("READY ", prefix: true);
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public static readonly string Stopped = Marker("STOPPED");
    public static readonly string RelocationRequested = Marker("RELOCATION_REQUESTED");
    public static readonly string Relocated = Marker("RELOCATED");
    public static readonly string SafeToShutdown = Marker("SAFE_TO_SHUTDOWN");
}
