namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-world";
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

    public static string FieldId(int x)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(x);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(x, FieldWidth * FieldCount - 1);
        return $"{FieldPrefix}{x / FieldWidth + 1}";
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

    public static string DungeonId(string partyId) => $"dungeon-{partyId}";
}

public static class Rejection
{
    public const string InvalidParty = "INVALID_PARTY";
    public const string NotLeader = "NOT_LEADER";
}

public sealed record JoinWorldRequest(string PlayerId, int X);

public sealed record MoveRequest(int X, int Sequence);

public sealed record EnterField(int X, int Sequence);

public sealed record WorldState(string FieldId, string NodeId, int X, int Sequence);

public sealed record BorderPresence(string SourceFieldId, string PlayerId, int X, int Sequence);

public sealed record BorderVisible(
    string ObserverFieldId,
    string SourceFieldId,
    string PlayerId,
    int X,
    int Sequence
);

public sealed record CreatePartyRequest(string PartyId);

public sealed record JoinPartyRequest(string PartyId);

public sealed record EnterDungeonRequest;

public sealed record AdvanceRequest;

public sealed record ViewRequest;

public sealed record PartyView(string PartyId, string[] Members);

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
    int Stage
);

public sealed record PrepareDungeon(string PartyId, string[] Members);

public sealed record DungeonPrepared(string DungeonId);

public sealed record JoinDungeon(string DungeonId, string PartyId);

public sealed record ActionRejected(string Reason);

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);

public sealed record NodeReady(string Id, string MeshEndpoint, string? StreamEndpoint);

public static class NodeProtocol
{
    public const string ReadyPrefix = "READY ";
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public const string Stopped = "STOPPED";
    public const string RelocationRequested = "RELOCATION_REQUESTED";
    public const string Relocated = "RELOCATED";
    public const string SafeToShutdown = "SAFE_TO_SHUTDOWN";
}
