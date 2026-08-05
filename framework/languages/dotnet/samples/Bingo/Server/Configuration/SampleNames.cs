using Bingo.Shared.Contracts;

namespace Bingo.Server.Configuration;

public static class SampleNames
{
    public const string PlayMeshName = "bingo.play";
    public const string MatchmakingMeshName = "bingo.matchmaking";
    public const string ApiChannel = "bingo.api";
    public const string RoomChannel = "bingo.room";
    public const string MatchmakerSpotType = "bingo.matchmaker";
    public const string RouterChannel = "bingo.gateway";
    public const string RewardTopic = "bingo.room.reward";
    public const string StreamNode = "bingo.client.stream";
    public const string SessionSpotNode = "bingo.session.node";
    public const string PlayerActorType = "bingo.player";
    public const string RoomSpotType = "bingo.room";
    public const string RoomSpotNode = "bingo.room.node";

    public static readonly string[] ActorIds =
    [
        BingoSamplePlayers.Player1,
        BingoSamplePlayers.Player2,
        BingoSamplePlayers.Observer
    ];

    public const string PlayerJoinedPacket = nameof(PlayerJoinedNotify);
    public const string GameStartedPacket = nameof(BingoGameStartedNotify);
    public const string NumberDrawnPacket = nameof(BingoNumberDrawnNotify);
    public const string GameEndedPacket = nameof(BingoGameEndedNotify);
    public const string RewardAnnouncedPacket = nameof(BingoRewardAnnouncedNotify);
}

public static class SampleTimings
{
    public static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(30);
}
