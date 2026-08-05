namespace Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;

internal sealed class ZlinkStreamLz4CompressionCodec : IZlinkStreamCompressionCodec
{
    public ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload)
    {
        return ZlinkStreamLz4PayloadCodec.Compress(payload);
    }

    public ReadOnlyMemory<byte> Decompress(ReadOnlyMemory<byte> payload, int maxDecompressedPayloadSize)
    {
        if (!ZlinkStreamLz4PayloadCodec.TryDecompress(
                payload,
                maxDecompressedPayloadSize,
                out var decompressed))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameTooLarge,
                "LZ4 decoded payload exceeds MaxReceivePayloadSize.");

        return decompressed;
    }
}
