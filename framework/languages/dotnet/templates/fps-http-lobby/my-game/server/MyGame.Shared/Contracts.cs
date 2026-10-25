namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-arena";
    public const string SessionStream = "players";
    public const string PlayerType = "Player";
    public const string RoomType = "Room";
    public const string MatchmakingType = "Matchmaking";
    public const string MatchmakingId = "matchmaking";
    public const string DefaultMode = "duel";
    public const string RoomPrefix = "room-";
    public const int PlayersPerRoom = 2;
    public const int InitialHp = 100;
    public const int ShotDamage = 25;
    public const double ShotRange = 80;
    public const double MaxStep = 2;
    public const double MaxSpeed = 10;
    public const int FireIntervalMs = 250;
    public const double EyeHeight = 1.6;
    public const double PlayerHalfWidth = 0.5;
    public const double PlayerHeight = 2;
}

public static class Rejection
{
    public const string BlockedByWall = "blocked-by-wall";
    public const string AdmissionRejected = "admission-rejected";
    public const string VerticalMove = "vertical-move";
    public const string NonFinite = "non-finite";
    public const string StepTooLarge = "step-too-large";
    public const string OutsideMap = "outside-map";
    public const string MoveBlockedByWall = "move-blocked-by-wall";
    public const string MoveBudget = "move-budget";
    public const string InvalidAim = "invalid-aim";
    public const string FireTooSoon = "fire-too-soon";
    public const string StaleSequence = "stale-sequence";
    public const string MatchFinished = "match-finished";
    public const string PacketNotAllowed = "packet-not-allowed";
}

public static class ShotOutcome
{
    public const string Hit = "hit";
    public const string Miss = "miss";
    public const string BlockedByWall = Rejection.BlockedByWall;
}

public enum MatchPhase { Waiting, Playing, Finished }

public sealed record SessionInfoRequest;
public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);
public sealed record QueueRequest(string PlayerId, string Mode = GameNames.DefaultMode);
public sealed record RoomAdmission(string PlayerId, string RoomId);
public sealed record RoomAssignment(string RoomId);
public sealed record MatchFound(string RoomId);
public sealed record MoveRequest(Vec3 Delta, long Sequence);
public sealed record MoveResult(Vec3 Position, long Sequence);
public sealed record ShootRequest(Vec3 AimDirection, long Sequence);
public sealed record RoomViewRequest;
public sealed record PlayerView(string PlayerId, Vec3 Position, int Hp, long LastSequence);
public sealed record RoomView(string RoomId, string NodeId, PlayerView[] Players,
    MatchPhase Phase, string? Winner);
public sealed record ShotResult(string Outcome, string? TargetId, long Sequence);
public sealed record ShotRejected(string Reason, int RetryAfterMs = 0);
public sealed record ActionRejected(string Reason, int RetryAfterMs = 0);
public sealed record MatchEnded(string Winner);

public sealed record NodeReady(string Id, string MeshEndpoint, string? StreamEndpoint);

public static class LifecycleTiming
{
    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan ProcessObservation = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeObservation = MessageFollow + ProcessObservation;
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
    public static readonly string RelocationRequested = Marker("RELOCATION_REQUESTED");
    public static readonly string Relocated = Marker("RELOCATED");
    public static readonly string SafeToShutdown = Marker("SAFE_TO_SHUTDOWN");
}
