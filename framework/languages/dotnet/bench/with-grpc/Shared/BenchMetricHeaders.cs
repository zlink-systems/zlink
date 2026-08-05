using System.Buffers.Binary;
using System.Diagnostics;
using Google.Protobuf;

namespace WithGrpcBench.Shared;

public enum BenchPhase : byte
{
    Warmup = 0,
    Active = 1
}

public readonly record struct BenchMetricHeader(
    uint RunId,
    BenchPhase Phase,
    int PayloadSize,
    ulong Sequence,
    long SentTimestampNs);

public static class BenchMetricHeaders
{
    public const int HeaderSize = 29;
    private const uint Magic = 0x5A4C4E4B; // "ZLNK"

    public static BenchPayload CreatePayload(int payloadSize, uint runId, BenchPhase phase, ulong sequence)
    {
        return new BenchPayload
        {
            Body = UnsafeByteOperations.UnsafeWrap(CreatePayloadBytes(payloadSize, runId, phase, sequence))
        };
    }

    public static byte[] CreatePayloadBytes(int payloadSize, uint runId, BenchPhase phase, ulong sequence)
    {
        if (payloadSize < HeaderSize)
        {
            throw new ArgumentOutOfRangeException(
                nameof(payloadSize),
                payloadSize,
                $"Payload size must be at least {HeaderSize} bytes.");
        }

        var bytes = BenchPayloads.CreateBytes(payloadSize);
        Stamp(bytes, runId, phase, payloadSize, sequence, NowNs());
        return bytes;
    }

    public static void FillPayload(Span<byte> payload, uint runId, BenchPhase phase, ulong sequence)
    {
        if (payload.Length < HeaderSize)
        {
            throw new ArgumentOutOfRangeException(
                nameof(payload),
                payload.Length,
                $"Payload size must be at least {HeaderSize} bytes.");
        }

        payload.Fill(0xab);
        Stamp(payload, runId, phase, payload.Length, sequence, NowNs());
    }

    public static bool TryDecode(BenchPayload payload, out BenchMetricHeader header)
    {
        return TryDecode(payload.Body.Span, out header);
    }

    public static bool IsExpected(
        BenchMetricHeader header,
        uint runId,
        BenchPhase phase,
        int payloadSize,
        ulong sequence)
    {
        return header.RunId == runId
            && header.Phase == phase
            && header.PayloadSize == payloadSize
            && header.Sequence == sequence;
    }

    public static long NowNs()
    {
        return Stopwatch.GetTimestamp() * 1_000_000_000L / Stopwatch.Frequency;
    }

    private static void Stamp(
        Span<byte> payload,
        uint runId,
        BenchPhase phase,
        int payloadSize,
        ulong sequence,
        long sentTimestampNs)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(payload[0..4], Magic);
        BinaryPrimitives.WriteUInt32LittleEndian(payload[4..8], runId);
        payload[8] = (byte)phase;
        BinaryPrimitives.WriteUInt32LittleEndian(payload[9..13], (uint)payloadSize);
        BinaryPrimitives.WriteUInt64LittleEndian(payload[13..21], sequence);
        BinaryPrimitives.WriteInt64LittleEndian(payload[21..29], sentTimestampNs);
    }

    public static bool TryDecode(ReadOnlySpan<byte> payload, out BenchMetricHeader header)
    {
        header = default;
        if (payload.Length < HeaderSize)
        {
            return false;
        }

        var magic = BinaryPrimitives.ReadUInt32LittleEndian(payload[0..4]);
        if (magic != Magic)
        {
            return false;
        }

        header = new BenchMetricHeader(
            BinaryPrimitives.ReadUInt32LittleEndian(payload[4..8]),
            (BenchPhase)payload[8],
            (int)BinaryPrimitives.ReadUInt32LittleEndian(payload[9..13]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[13..21]),
            BinaryPrimitives.ReadInt64LittleEndian(payload[21..29]));
        return true;
    }
}
