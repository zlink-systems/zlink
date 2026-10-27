namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-arena";
    public const string SessionStream = "players";
    public const string PlayerType = "Player";
    public const string RoomType = "Room";
    public const string DefaultMode = "duel";
    public const string MatchPath = "/match";
    public const string RoomPrefix = "room-";
    public const long MaxSafeInteger = 9_007_199_254_740_991L;
    public const int PlayersPerRoom = 2;
    public const int InitialHp = 100;
    public const int ShotDamage = 25;
    public const double ShotRange = 80;
    public const double MapMinX = 0;
    public const double MapMaxX = 100;
    public const double MapMinY = 0;
    public const double MapMaxY = 100;
    public const double MaxStep = 2.0;
    public const double MaxSpeed = 10.0;
    public const int FireIntervalMs = 250;
    public const double FirstSpawnX = 40;
    public const double FirstSpawnY = 10;
    public const double SecondSpawnX = 60;
    public const double SecondSpawnY = 10;
    public const double WallX = 50;
    public const double WallMinY = 20;
    public const double WallMaxY = 60;
    public const string PlayingPhase = "Playing";
    public const string FinishedPhase = "Finished";
}

public static class Rejection
{
    public const string OutOfRange = "out-of-range";
    public const string BlockedByWall = "blocked-by-wall";
    public const string NoTarget = "no-target";
    public const string AdmissionRejected = "admission-rejected";
    public const string InvalidMove = "invalid-move";
    public const string StaleSequence = "stale-sequence";
    public const string MatchFinished = "match-finished";
    public const string StepTooLarge = "step-too-large";
    public const string OutsideMap = "outside-map";
    public const string MoveBlockedByWall = "move-blocked-by-wall";
    public const string MoveBudget = "move-budget";
    public const string FireTooSoon = "fire-too-soon";
    public const string IntegerOutOfRange = "integer-out-of-range";
    public const string PacketNotAllowed = "packet-not-allowed";
}

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);

public sealed record MatchRequest(string PlayerId, string Mode = GameNames.DefaultMode);

public sealed record MatchResponse(string RoomId, string SessionEndpoint);

public sealed record JoinRoomRequest(string PlayerId, string RoomId);

public sealed record PrepareRoom(string MatchId, string[] Members);

public sealed record RoomPrepared(string MatchId);

public sealed record MoveRequest(double Dx, double Dy, long Sequence);

public sealed record ShootRequest(string TargetId, long Sequence);

public sealed record MoveResult(double X, double Y, long Sequence);

public sealed record ShotResult(string Outcome, string TargetId, long Sequence);

public sealed record RoomViewRequest;

public sealed record PlayerView(string PlayerId, double X, double Y, int Hp, long LastSequence);

public sealed record RoomView(
    string RoomId,
    string NodeId,
    string Phase,
    string? Winner,
    PlayerView[] Players
);

public sealed record ActionRejected(string Reason, int RetryAfterMs = 0);

public sealed record MatchEnded(string Winner);

public sealed record NodeReady(
    string Id,
    string MeshEndpoint,
    string? StreamEndpoint,
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
    public static readonly TimeSpan RelocationDeadline = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeShutdownDeadline = MessageFollow + RelocationDeadline;
    public static readonly string ReadyPrefix = Marker("READY ", prefix: true);
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public static readonly string RelocationRequested = Marker("RELOCATION_REQUESTED");
    public static readonly string Relocated = Marker("RELOCATED");
    public static readonly string SafeToShutdown = Marker("SAFE_TO_SHUTDOWN");
}
