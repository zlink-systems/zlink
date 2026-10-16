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

public sealed record MatchRequest(string PlayerId, string Mode = GameNames.DefaultMode);

public sealed record MatchResponse(string RoomId, string SessionEndpoint);

public sealed record JoinRoomRequest(string PlayerId, string RoomId);

public sealed record MoveRequest(double X, double Y, long Sequence);

public sealed record ShootRequest(string TargetId, long Sequence);

public sealed record RoomViewRequest;

public sealed record PlayerView(string PlayerId, double X, double Y, int Hp);

public sealed record RoomView(string RoomId, string NodeId, PlayerView[] Players);

public sealed record ShotRejected(string Reason);

public sealed record ActionRejected(string Reason);

public sealed record MatchEnded(string Winner);

public sealed record NodeReady(
    string Id,
    string MeshEndpoint,
    string? StreamEndpoint,
    string? HttpEndpoint = null
);

public static class NodeProtocol
{
    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan RelocationDeadline = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeShutdownDeadline = MessageFollow + RelocationDeadline;
    public const string ReadyPrefix = "READY ";
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public const string RelocationRequested = "RELOCATION_REQUESTED";
    public const string Relocated = "RELOCATED";
    public const string SafeToShutdown = "SAFE_TO_SHUTDOWN";
}
