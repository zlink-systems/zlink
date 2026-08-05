using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamProtocolDefaults
{
    private const int DefaultMaxDecompressedPayloadSize = 64 * 1024;

    public static IZlinkStreamPacketNameResolver PacketNameResolver { get; } =
        new ZlinkStreamPacketNameResolver();

    public static ReadOnlyMemory<byte> EncodeHeader(ZlinkStreamHeader header)
    {
        return ZLinkStreamHeaderCodec.Encode(header);
    }

    public static ZlinkStreamHeader DecodeHeader(ReadOnlyMemory<byte> header)
    {
        return ZLinkStreamHeaderCodec.Decode(header);
    }

    public static IZlinkStreamCompressionCodec CreateLz4CompressionCodec()
    {
        return new ZLinkLz4StreamCompressionCodec();
    }

    public static ReadOnlyMemory<byte> Compress(
        IZlinkStreamCompressionCodec? compressionCodec,
        ReadOnlyMemory<byte> payload)
    {
        if (compressionCodec is null) throw new InvalidOperationException("Compression codec is not configured.");

        return compressionCodec.Compress(payload);
    }

    public static ReadOnlyMemory<byte> Decompress(
        IZlinkStreamCompressionCodec? compressionCodec,
        ReadOnlyMemory<byte> payload,
        int maxDecompressedPayloadSize = DefaultMaxDecompressedPayloadSize)
    {
        if (compressionCodec is null) throw new InvalidOperationException("Compression codec is not configured.");

        var decompressed = compressionCodec.Decompress(payload, maxDecompressedPayloadSize);
        if (decompressed.Length > maxDecompressedPayloadSize)
            throw new InvalidOperationException("Decoded stream payload exceeds maximum stream payload size.");

        return decompressed;
    }
}
