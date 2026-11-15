using System.Text.Json;
using Systems.Zlink.Stream.Connector.Internal;

namespace WithGrpcBench.Shared;

// Declaration order is the shared fixture's Framework JSON v1 wire order.
// Required init properties use the Framework's declared-nullability setter checks.
public sealed record BenchGameMessage
{
    public required string DisplayName { get; init; }
    public required long Gold { get; init; }
    public required int Heading { get; init; }
    public required long Hp { get; init; }
    public required IReadOnlyList<BenchGameItem> Items { get; init; }
    public required long Level { get; init; }
    public required long Mp { get; init; }
    public required bool Online { get; init; }
    public required long Phase { get; init; }
    public required string PlayerId { get; init; }
    public required string RoomId { get; init; }
    public required long Run { get; init; }
    public required long SentNs { get; init; }
    public required long Seq { get; init; }
    public required string Session { get; init; }
    public required int X { get; init; }
    public required int Y { get; init; }
    public required int Z { get; init; }
}

public sealed record BenchGameItem
{
    public required int Count { get; init; }
    public required string Id { get; init; }
    public required int Slot { get; init; }
}

public static class BenchGamePayloads
{
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
        new()
        {
            DisplayName = "Player_12345678",
            Gold = 987654321,
            Heading = 271,
            Hp = 287,
            Items = Enumerable
                .Range(0, itemCount)
                .Select(index => new BenchGameItem
                {
                    Count = index % 9 + 1,
                    Id = $"item-{index + 1:D8}",
                    Slot = index % 32,
                })
                .ToArray(),
            Level = 42,
            Mp = 164,
            Online = true,
            Phase = (long)phase,
            PlayerId = "player-0000000000000001",
            RoomId = "room-000000000000000001",
            Run = runId,
            SentNs = sentNs,
            Seq = checked((long)sequence),
            Session = "session-0000000000000001",
            X = -123456,
            Y = 7890,
            Z = 654321,
        };

    public static byte[] EncodeStandard(BenchGameMessage value) =>
        JsonSerializer.SerializeToUtf8Bytes(value, ZLinkJsonSerializerOptions.FrameworkPayload);

    public static BenchGameMessage DecodeStandard(ReadOnlySpan<byte> value)
    {
        ZLinkFrameworkJsonDocument.ValidateDocument(value);
        return JsonSerializer.Deserialize<BenchGameMessage>(
                value,
                ZLinkJsonSerializerOptions.FrameworkPayload
            ) ?? throw new JsonException("Game JSON body decoded to null.");
    }

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
