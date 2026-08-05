using Google.Protobuf;

namespace WithGrpcBench.Shared;

public static class BenchPayloads
{
    public static BenchPayload Create(int payloadSize)
    {
        return new BenchPayload { Body = UnsafeByteOperations.UnsafeWrap(CreateBytes(payloadSize)) };
    }

    public static byte[] CreateBytes(int payloadSize)
    {
        var bytes = new byte[payloadSize];
        for (var i = 0; i < bytes.Length; i++)
        {
            bytes[i] = (byte)((i * 31 + 17) & 0xff);
        }

        return bytes;
    }

    public static void Validate(BenchPayload payload, int expectedSize)
    {
        if (payload.Body.Length != expectedSize)
        {
            throw new InvalidOperationException(
                $"Unexpected payload size. expected={expectedSize}, actual={payload.Body.Length}");
        }
    }
}
