namespace MyGame.Shared;

public static class LobbyNames
{
    public const string MatchPath = "/match";
}

public sealed record MatchRequest(string PlayerId, string Mode = GameNames.DefaultMode);
public sealed record MatchResponse(string RoomId, string SessionEndpoint);
public sealed record JoinRoomRequest(string PlayerId, string RoomId);
public sealed record PrepareRoom(string MatchId, string[] Members);
public sealed record RoomPrepared(string MatchId);
public sealed record HttpNodeReady(string Id, string MeshEndpoint, string? StreamEndpoint,
    string? HttpEndpoint = null);
