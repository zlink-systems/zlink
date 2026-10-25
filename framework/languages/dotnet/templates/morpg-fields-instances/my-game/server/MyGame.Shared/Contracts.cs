using System.Text.Json.Serialization;

namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-world";
    public const long MaxSafeInteger = 9_007_199_254_740_991;
    public const int FinalStage = 3;
    public const string PartyType = "Party";
    public const string FieldType = "Field";
    public const string PlayerType = "Player";
    public const string DungeonType = "Dungeon";
    public const string SessionStream = "players";
    public const int MaximumPartyMembers = 4;
    public const string BorderChannel = "border";
    public const string BorderTimer = "border-tick";
    private const string FieldPrefix = "field-";
    public const int FieldWidth = 100;
    public const int FieldCount = 4;
    public const int BorderRadius = 10;
    public const int TickPeriodMilliseconds = 100;

    public static string FieldId(double x)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(x);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(x, FieldWidth * FieldCount);
        return $"{FieldPrefix}{(int)(x / FieldWidth) + 1}";
    }

    public static IEnumerable<string> AdjacentFields(string fieldId)
    {
        var number = int.Parse(fieldId.AsSpan(FieldPrefix.Length));
        if (number > 1)
            yield return $"{FieldPrefix}{number - 1}";
        if (number < FieldCount)
            yield return $"{FieldPrefix}{number + 1}";
    }

    public static string BorderTopic(string source, string target) => $"{source}:{target}";

    public static string DungeonId(string runId) => $"dungeon-{runId}";
}

public static class GameTiming
{
    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan Relocation = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeShutdown = MessageFollow + Relocation;
}

public static class Rejection
{
    public const string InvalidParty = "invalid-party";
    public const string NotLeader = "not-leader";
}

public sealed record JoinWorldRequest(string PlayerId, double X);

public sealed record MoveRequest(double Delta, long Sequence);

public sealed record EnterField(double X, long Sequence, int RemainingMs = 0);

public sealed record WorldState(string FieldId, string NodeId, double X, long Sequence);

public sealed record BorderPresence(string SourceFieldId, string PlayerId, double X, long Sequence);

public sealed record BorderVisible(
    string ObserverFieldId,
    string SourceFieldId,
    string PlayerId,
    double X,
    long Sequence
);

public sealed record CreatePartyRequest;

public sealed record JoinPartyRequest(string PartyId);

public sealed record EnterDungeonRequest;

public sealed record AdvanceRequest;

public sealed record ViewRequest;

[JsonConverter(typeof(JsonStringEnumConverter))]
public enum RunPhase { Forming, Preparing, Entering, Active, Completed }

public sealed record PartyView(string PartyId, string[] Members, string LeaderId, RunPhase Phase, string? RunId);
public sealed record PartyCommand(string ActorId, bool Join = false, bool Enter = false);
public sealed record PartyCommandResult(PartyView? View, ActionRejected? Rejection);
public sealed record DungeonMemberEntered(string RunId, string ActorId);
public sealed record RunStarted(string RunId);
public sealed record RunCompleted(string RunId);

public sealed record FieldView(
    string FieldId,
    string NodeId,
    string[] Players,
    PartyView[] Parties
);

public sealed record DungeonView(
    string DungeonId,
    string NodeId,
    string PartyId,
    string[] Members,
    int Stage,
    string RunId,
    RunPhase Phase
);

public sealed record PrepareDungeon(string RunId, string PartyId, string[] Members);

public sealed record DungeonPrepared(string DungeonId, string RunId, ActionRejected? Rejection = null);

public sealed record JoinDungeon(string DungeonId, string RunId);

public sealed record ActionRejected(string Reason, int RetryAfterMs = 0);

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);

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

    public static readonly string ReadyPrefix = Marker("READY ", prefix: true);
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public static readonly string Stopped = Marker("STOPPED");
    public static readonly string RelocationRequested = Marker("RELOCATION_REQUESTED");
    public static readonly string Relocated = Marker("RELOCATED");
    public static readonly string SafeToShutdown = Marker("SAFE_TO_SHUTDOWN");
}
