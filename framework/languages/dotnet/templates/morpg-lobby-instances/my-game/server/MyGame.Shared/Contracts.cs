namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-world";
    public const string PartyType = "Party";
    public const string PlayerType = "Player";
    public const string DungeonType = "Dungeon";
    public const string SessionStream = "players";
    public const int MaximumPartyMembers = 4;
    public static string PartyId(string partyId) => $"party-{partyId}";
    public static string DungeonId(string partyId) => $"dungeon-{partyId}";
}
public static class Rejection
{
    public const string JoinRejected = "join-rejected";
    public const string InvalidParty = "invalid-party";
    public const string NotLeader = "not-leader";
    public const string NotReady = "not-ready";
}
public sealed record JoinLobbyRequest(string PlayerId);
public sealed record LobbyView;
public sealed record CreatePartyRequest(string PartyId);
public sealed record JoinPartyRequest(string PartyId);
public sealed record PartyJoin(string PartyId, bool Create);
public sealed record ReadyRequest;
public sealed record EnterDungeonRequest;
public sealed record AdvanceRequest;
public sealed record ViewRequest;
public sealed record PartyView(string PartyId, string[] Members, bool[] Ready);
public sealed record DungeonView(string DungeonId, string NodeId, string PartyId, string[] Members, int Stage);
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
