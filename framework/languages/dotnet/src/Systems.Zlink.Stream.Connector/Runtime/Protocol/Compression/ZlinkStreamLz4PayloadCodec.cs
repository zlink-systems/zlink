using K4os.Compression.LZ4;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;

internal static class ZlinkStreamLz4PayloadCodec
{
    public static ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload)
    {
        return LZ4Pickler.Pickle(payload.Span);
    }

    public static bool TryDecompress(
        ReadOnlyMemory<byte> payload,
        int maxDecompressedPayloadSize,
        out ReadOnlyMemory<byte> decompressed)
    {
        if (LZ4Pickler.UnpickledSize(payload.Span) > maxDecompressedPayloadSize)
        {
            decompressed = default;
            return false;
        }

        decompressed = LZ4Pickler.Unpickle(payload.Span);
        return true;
    }
}
