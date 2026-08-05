namespace TicTacToe.Shared.Contracts;

public static class TicTacToeMarks
{
    public const string X = "X";
    public const string O = "O";
}

public static class TicTacToeGameStatuses
{
    public const string WaitingForPlayers = "WaitingForPlayers";
    public const string InProgress = "InProgress";
    public const string Won = "Won";
    public const string Draw = "Draw";
    public const string TurnTimedOut = "TurnTimedOut";
}

public sealed record PlayerInfo(
    string ActorId,
    string DisplayName,
    int Level,
    int Wins);

public sealed record PlayNodeInfo(string StreamEndpoint);

public sealed record CreateGameHttpReq(string? GameName);

public sealed record CreateGameHttpRes(
    string RoomId,
    IReadOnlyList<string> PlayEndpoints,
    IReadOnlyList<PlayNodeInfo> PlayNodes,
    string GameName,
    int RequiredLevel);

public sealed record TicTacToeGameCreateReq(
    string GameName,
    int RequiredLevel);

public sealed record AuthenticatePlayerReq(string AccessToken);

public sealed record AuthenticatePlayerRes(PlayerInfo Player);

public sealed record AuthenticateReq(string AccessToken);

public sealed record AuthenticateRes(PlayerInfo Player);

public sealed record TicTacToeGameJoinReq(
    string RoomId,
    PlayerInfo Player);

public sealed record TicTacToeGameJoinRes(GameState State);

public sealed record JoinGameReq(string RoomId);

public sealed record JoinGameRes(GameState State);

public sealed record JoinGameFailedNotify(
    string RoomId,
    string Error);

public sealed record ObserveMilestoneReq;

public sealed record ObserveMilestoneRes(bool Subscribed);

public sealed record PlaceMarkReq(int Cell);

public sealed record PlaceMarkRes(GameState State);

public sealed record LeaveGameMsg(string RoomId);

public sealed record LeaveGameRes(bool Completed);

public sealed record PlayerJoinedNotify(
    string RoomId,
    string ActorId,
    string DisplayName,
    int Level,
    string Mark,
    GameState State);

public sealed record GameStateNotify(GameState State);

public sealed record WinMilestoneNotify(
    string RoomId,
    string ActorId,
    string DisplayName,
    int Wins);

public sealed record PlayerWinMilestoneEvent(
    string RoomId,
    string ActorId,
    string DisplayName,
    int Wins);

public sealed record GameState(
    string RoomId,
    string Board,
    string Status,
    string? Winner,
    string NextTurn,
    string? XActorId,
    string? OActorId,
    string? LastMoveActorId,
    int? LastMoveCell);
