using System.Text.Json;
using System.Text.Json.Serialization;

namespace WithGrpcBench.Shared;

// Declaration order is the shared fixture's Framework JSON v1 wire order.
public sealed record BenchGameMessage(
    string DisplayName,
    [property: JsonNumberHandling(
        JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString
    )]
        long Gold,
    int Heading,
    [property: JsonNumberHandling(
        JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString
    )]
        long Hp,
    IReadOnlyList<BenchGameItem> Items,
    [property: JsonNumberHandling(
        JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString
    )]
        long Level,
    [property: JsonNumberHandling(
        JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString
    )]
        long Mp,
    bool Online,
    [property: JsonNumberHandling(
        JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString
    )]
        long Phase,
    string PlayerId,
    string RoomId,
    [property: JsonNumberHandling(
        JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString
    )]
        long Run,
    [property: JsonNumberHandling(
        JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString
    )]
        long SentNs,
    [property: JsonNumberHandling(
        JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString
    )]
        long Seq,
    string Session,
    int X,
    int Y,
    int Z
);

public sealed record BenchGameItem(int Count, string Id, int Slot);

public static class BenchGamePayloads
{
    private static readonly JsonSerializerOptions RawJsonOptions = new(JsonSerializerDefaults.Web);
    private const long TimestampOffsetNs = 1_000_000_000_000_000_000L;

    public static long TimestampNow() => checked(TimestampOffsetNs + BenchMetricHeaders.NowNs());

    public static int ItemCount(int payloadSize) =>
        payloadSize switch
        {
            1024 => 16,
            4096 => 88,
            _ => throw new ArgumentOutOfRangeException(
                nameof(payloadSize),
                payloadSize,
                $"zlink-dotnet-json and zlink-framework-dotnet-json support payload sizes 1024 and 4096 only. Received {payloadSize}."
            ),
        };

    public static BenchGameMessage Create(
        uint runId,
        BenchPhase phase,
        ulong sequence,
        int payloadSize
    ) => Create(runId, phase, sequence, TimestampNow(), ItemCount(payloadSize));

    public static BenchGameMessage Create(
        uint runId,
        BenchPhase phase,
        ulong sequence,
        long sentNs,
        int itemCount
    ) =>
        new(
            "Player_12345678",
            987654321,
            271,
            287,
            Enumerable
                .Range(0, itemCount)
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

    public static byte[] EncodeStandard(BenchGameMessage value) =>
        JsonSerializer.SerializeToUtf8Bytes(value, RawJsonOptions);

    public static BenchGameMessage DecodeStandard(ReadOnlySpan<byte> value) =>
        JsonSerializer.Deserialize<BenchGameMessage>(value, RawJsonOptions)
        ?? throw new InvalidOperationException("Game JSON body decoded to null.");

    public static bool HasExpectedStamp(
        BenchGameMessage value,
        uint runId,
        BenchPhase phase,
        ulong sequence
    ) =>
        value.Run == runId
        && value.Phase == (long)phase
        && value.Seq == checked((long)sequence)
        && value.SentNs > 0;
}
