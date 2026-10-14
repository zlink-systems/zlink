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
    public static string TownId(int channel)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(channel, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(channel, ChannelCount);
        return $"town-{channel}";
    }
    public static string DungeonId(string partyId) => $"dungeon-{partyId}";
}
public static class Rejection
{
    public const string JoinRejected = "JOIN_REJECTED";
    public const string InvalidParty = "INVALID_PARTY";
    public const string NotLeader = "NOT_LEADER";
}
public sealed record JoinTownRequest(string PlayerId, int Channel);
public sealed record ChangeChannelRequest(int Channel);
public sealed record CreatePartyRequest(string PartyId);
public sealed record JoinPartyRequest(string PartyId);
public sealed record EnterDungeonRequest;
public sealed record AdvanceRequest;
public sealed record ViewRequest;
public sealed record PartyView(string PartyId, string[] Members);
public sealed record TownView(string TownId, string NodeId, string[] Players, PartyView[] Parties);
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
