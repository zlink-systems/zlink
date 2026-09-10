using Google.Protobuf;

namespace WithGrpcBench.Shared;

public static class RawWire
{
    public static void Encode(BenchPayload payload, Span<byte> destination) => payload.WriteTo(destination);

    public static BenchPayload Decode(ReadOnlySpan<byte> encoded) => BenchPayload.Parser.ParseFrom(encoded);
}
