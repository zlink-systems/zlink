using System;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    public interface IZlinkStreamPacketNameResolver
    {
        string Resolve(Type payloadType);
    }

    /// <summary>
    ///     Converts a business object to and from a <see cref="ZlinkStreamEncodedPayload" />.
    ///     Set <see cref="ZlinkStreamConnectorOptions.PayloadCodec" /> to plug a codec into the
    ///     typed connector send/request/observe API.
    /// </summary>
    /// <remarks>
    ///     Unlike the native .NET connector, this adapter has no built-in JSON codec: Unity
    ///     does not ship System.Text.Json, and bundling a serializer into a connector adapter
    ///     would decide the serializer for the game. The typed API therefore requires this
    ///     option and throws <see cref="NotSupportedException" /> when it is not set.
    /// </remarks>
    public interface IZlinkStreamPayloadCodec
    {
        /// <summary>Encodes <paramref name="payload" /> into a wire payload.</summary>
        ZlinkStreamEncodedPayload Encode<TPayload>(TPayload payload);

        /// <summary>Decodes a wire payload back into an instance of <typeparamref name="TPayload" />.</summary>
        TPayload Decode<TPayload>(ZlinkStreamEncodedPayload payload);
    }

    /// <summary>
    ///     Describes the STREAM wire codec supplied by a payload codec extension.
    /// </summary>
    public interface IZlinkStreamCodecRegistration
    {
        string ContentType { get; }

        ZlinkStreamCodec Codec { get; }
    }

    /// <summary>
    ///     Transforms encoded stream payload bytes when a send or request call opts into
    ///     compression.
    /// </summary>
    /// <remarks>
    ///     WebGL runs the compression codec the TypeScript connector was built with, which
    ///     is selected by <see cref="ZlinkStreamConnectorOptions.Compression" />. A managed
    ///     codec cannot be handed across the jslib boundary, so setting
    ///     <see cref="ZlinkStreamConnectorOptions.CompressionCodec" /> is rejected.
    /// </remarks>
    public interface IZlinkStreamCompressionCodec
    {
        ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload);

        ReadOnlyMemory<byte> Decompress(ReadOnlyMemory<byte> payload, int maxDecompressedPayloadSize);
    }
}
