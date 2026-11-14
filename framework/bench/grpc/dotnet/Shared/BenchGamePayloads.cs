namespace WithGrpcBench.Shared;

// Property declaration order is the fixture's Framework JSON v1 wire order.
public sealed record BenchGameMessage(
    string DisplayName,
    long Gold,
    int Heading,
    long Hp,
    IReadOnlyList<BenchGameItem> Items,
    long Level,
    long Mp,
    bool Online,
    long Phase,
    string PlayerId,
    string RoomId,
    long Run,
    long SentNs,
    long Seq,
    string Session,
    int X,
    int Y,
    int Z
);

public sealed record BenchGameItem(int Count, string Id, int Slot);

public sealed record BenchPayloadInput(byte[] Bytes, BenchGameMessage? GameMessage);

public enum BenchPayloadShape
{
    Bytes,
    Mixed,
}

public static class BenchGamePayloads
{
    private const long TimestampOffsetNs = 1_000_000_000_000_000_000L;

    public static long TimestampNow() => checked(TimestampOffsetNs + BenchMetricHeaders.NowNs());

    public static BenchGameMessage Create(
        uint runId,
        BenchPhase phase,
        ulong sequence,
        long sentNs,
        int itemCount
    ) => new(
        "Player_12345678",
        987654321,
        271,
        287,
        Enumerable.Range(0, itemCount)
            .Select(index => new BenchGameItem(
                index % 9 + 1,
                $"item-{index + 1:D8}",
                index % 32
            ))
            .ToArray(),
        42,
        164,
        true,
        (long)phase,
        "player-0000000000000001",
        "room-000000000000000001",
        runId,
        sentNs,
        checked((long)sequence),
        "session-0000000000000001",
        -123456,
        7890,
        654321
    );

    public static (int ItemCount, int JsonSize) Plan(int payloadSize) => payloadSize switch
    {
        1024 => (16, 1011),
        4096 => (88, 4087),
        _ => throw new ArgumentOutOfRangeException(nameof(payloadSize), payloadSize, "Mixed payload supports 1024 and 4096 byte targets."),
    };

    public static int EncodedLength(int payloadSize, uint run, BenchPhase phase, ulong sequence)
    {
        var baseSize = Plan(payloadSize).JsonSize;
        var digitDelta = run.ToString(System.Globalization.CultureInfo.InvariantCulture).Length - 3
            + ((long)phase).ToString(System.Globalization.CultureInfo.InvariantCulture).Length - 1
            + sequence.ToString(System.Globalization.CultureInfo.InvariantCulture).Length - 3;
        return checked(baseSize + (int)digitDelta);
    }

}
