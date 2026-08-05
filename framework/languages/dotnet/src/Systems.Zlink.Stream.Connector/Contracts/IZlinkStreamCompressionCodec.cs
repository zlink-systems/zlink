namespace Systems.Zlink.Stream.Connector.Contracts;

/// <summary>
///     Transforms encoded stream payload bytes when a send or request call opts into
///     compression. The frame only records that payload bytes are compressed; both
///     endpoints must configure codecs with the same wire format.
/// </summary>
public interface IZlinkStreamCompressionCodec
{
    ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload);

    ReadOnlyMemory<byte> Decompress(ReadOnlyMemory<byte> payload, int maxDecompressedPayloadSize);
}