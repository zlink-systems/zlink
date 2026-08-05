using Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkLz4StreamCompressionCodec : IZlinkStreamCompressionCodec
{
    public static ReadOnlyMemory<byte> CompressPayload(ReadOnlyMemory<byte> payload)
    {
        return ZlinkStreamLz4PayloadCodec.Compress(payload);
    }

    public static ReadOnlyMemory<byte> DecompressPayload(
        ReadOnlyMemory<byte> payload,
        int maxDecompressedPayloadSize)
    {
        if (!ZlinkStreamLz4PayloadCodec.TryDecompress(
                payload,
                maxDecompressedPayloadSize,
                out var decompressed))
            throw new InvalidOperationException("LZ4 decoded stream payload exceeds maximum stream payload size.");

        return decompressed;
    }

    public ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload)
    {
        return CompressPayload(payload);
    }

    public ReadOnlyMemory<byte> Decompress(ReadOnlyMemory<byte> payload, int maxDecompressedPayloadSize)
    {
        return DecompressPayload(payload, maxDecompressedPayloadSize);
    }
}
