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
    public const double WallX = 50;
    public const double WallMinY = 0;
    public const double WallMaxY = 100;
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
}

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);

public sealed record QueueRequest(string PlayerId, string Mode = GameNames.DefaultMode);

public sealed record RoomAdmission(string PlayerId, string RoomId);

public sealed record RoomAssignment(string RoomId);

public sealed record MatchFound(string RoomId);

public sealed record MoveRequest(double X, double Y, long Sequence);

public sealed record ShootRequest(string TargetId, long Sequence);

public sealed record RoomViewRequest;

public sealed record PlayerView(string PlayerId, double X, double Y, int Hp);

public sealed record RoomView(string RoomId, string NodeId, PlayerView[] Players);

public sealed record ShotRejected(string Reason);

public sealed record ActionRejected(string Reason);

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
