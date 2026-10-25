namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "my-game-tables";
    public const string SessionStream = "players";
    public const string PlayerType = "Player";
    public const string TableType = "Table";
    public const int SeatCount = 4;
    public const long InitialChips = 1000;
    public const long MaxContractInteger = 9_007_199_254_740_991;
    public const int DeckSize = 52;
    public const int RankCount = 13;
    public const int LowestRank = 2;
    public const int CardsPerHand = 2;
    public const int MinimumPlayers = 2;
    public const int NoSeat = -1;

    public static bool IsContractInteger(long value) => value >= 0 && value <= MaxContractInteger;

    public static bool TryAddContractIntegers(long left, long right, out long sum)
    {
        if (
            !IsContractInteger(left)
            || !IsContractInteger(right)
            || right > MaxContractInteger - left
        )
        {
            sum = 0;
            return false;
        }

        sum = left + right;
        return true;
    }
}

public static class Rejection
{
    public const string TableFull = "table-full";
    public const string NoHand = "no-hand";
    public const string HandInProgress = "hand-in-progress";
    public const string NotEnoughPlayers = "not-enough-players";
    public const string NotYourTurn = "not-your-turn";
    public const string InvalidAmount = "invalid-amount";
    public const string NotSeated = "not-seated";
    public const string IntegerOutOfRange = "integer-out-of-range";
    public const string PacketNotAllowed = "packet-not-allowed";
}

public sealed record SessionInfoRequest;

public sealed record SessionInfo(string SessionId, string ActorId, ulong ObjectGeneration);

public sealed record JoinTableRequest(string PlayerId, string TableId);

public sealed record TableViewRequest;

public sealed record StartHandRequest;

public sealed record BetRequest(long HandNumber, long Amount);

public sealed record SeatView(int Seat, string PlayerId, long Chips, bool Acted, int CardCount);

public sealed record TableView(
    string TableId,
    string NodeId,
    int YourSeat,
    long HandNumber,
    bool InHand,
    int TurnSeat,
    long Pot,
    SeatView[] Seats
);

public sealed record HandDealt(long HandNumber, int[] Cards);

public sealed record RevealedHand(int Seat, string PlayerId, int[] Cards);

public sealed record HandResult(
    long HandNumber,
    long Pot,
    int[] Winners,
    RevealedHand[] Hands,
    SeatView[] Seats
);

public sealed record ActionRejected(string Reason, int RetryAfterMs);

public sealed record NodeReady(string Id, string MeshEndpoint, string? StreamEndpoint);

public static class HostTiming
{
    public static readonly TimeSpan Relocation = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan Safe = MessageFollow + Relocation;
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
