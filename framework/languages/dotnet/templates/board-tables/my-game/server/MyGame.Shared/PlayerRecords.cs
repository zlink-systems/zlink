namespace MyGame.Shared;

public static class PlayerRecords
{
    public const string Channel = "my-game-player-records";
}

public sealed record LoadPlayer(string PlayerId);

public sealed record PlayerRecord(string PlayerId, long Chips);

public sealed record CommitRound(string TableId, long HandNumber, PlayerRecord[] Results);

public sealed record RoundCommitted(string TableId, long HandNumber);
