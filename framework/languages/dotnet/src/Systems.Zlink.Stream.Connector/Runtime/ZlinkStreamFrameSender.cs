using System.Buffers;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamFrameSender(
    ZlinkStreamConnectorOptions options,
    ZlinkStreamHeaderCodec headerCodec,
    IZlinkStreamCompressionCodec? compressionCodec,
    SemaphoreSlim sendGate,
    Func<IZlinkStreamConnection?> connectionProvider
)
{
    public ZlinkStreamOutboundFrame BuildOutboundFrame(
        ZlinkStreamMessageKind kind,
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        ZlinkStreamRequestSeq? requestSeq,
        ushort? actorSlot = null
    )
    {
        var payloadBytes = payload.Payload;
        var flags = requestSeq is null
            ? ZlinkStreamHeaderFlags.None
            : ZlinkStreamHeaderFlags.HasRequestSeq;

        if (compress)
        {
            payloadBytes = CompressPayload(payloadBytes);
            flags |= ZlinkStreamHeaderFlags.PayloadCompressed;
        }

        ZlinkStreamFrameCodec.ValidateSendPayload(payloadBytes.Length, options.MaxSendPayloadSize);

        // Correlation ids link requests with their terminal replies.
        Span<char> correlationId = stackalloc char[16];
        var correlationLength = 0;
        if (kind == ZlinkStreamMessageKind.Request)
            // Every Int64, including counter wraparound, fits in 16 hex digits.
            ZlinkStreamCorrelation
                .NextValue()
                .TryFormat(correlationId, out correlationLength, "x");

        var header = new ZlinkStreamHeader(
            kind,
            payload.Codec,
            flags,
            requestSeq,
            name,
            metadata,
            null,
            null,
            null,
            actorSlot
        );
        return new ZlinkStreamOutboundFrame(
            headerCodec.Encode(header, correlationId[..correlationLength]),
            payloadBytes
        );
    }

    public async ValueTask SendControlAsync(string name, CancellationToken cancellationToken)
    {
        var frame = BuildOutboundFrame(
            ZlinkStreamMessageKind.Control,
            name,
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, ReadOnlyMemory<byte>.Empty),
            ZlinkStreamMetadata.Empty,
            false,
            null
        );
        ValidateSendReady(frame.HeaderBytes, frame.PayloadBytes);
        var connection =
            connectionProvider()
            ?? throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.Disconnected,
                "Connector is not connected."
            );
        await SendPacketAsync(connection, frame.HeaderBytes, frame.PayloadBytes, cancellationToken)
            .ConfigureAwait(false);
    }

    public void ValidateSendReady(ReadOnlyMemory<byte> header, ReadOnlyMemory<byte> payload)
    {
        ZlinkStreamFrameCodec.ValidateSendFrame(header.Length, payload.Length);
    }

    public async ValueTask SendPacketAsync(
        IZlinkStreamConnection connection,
        ReadOnlyMemory<byte> header,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await sendGate.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                if (connection.CanWriteSegments)
                {
                    var prefix = ArrayPool<byte>.Shared.Rent(6);
                    try
                    {
                        ZlinkStreamFrameCodec.WritePrefix(
                            prefix.AsSpan(0, 6),
                            header.Length,
                            payload.Length
                        );
                        await connection
                            .WriteAsync(prefix.AsMemory(0, 6), cancellationToken)
                            .ConfigureAwait(false);
                    }
                    finally
                    {
                        ArrayPool<byte>.Shared.Return(prefix);
                    }

                    if (header.Length > 0)
                        await connection
                            .WriteAsync(header, cancellationToken)
                            .ConfigureAwait(false);

                    if (payload.Length > 0)
                        await connection
                            .WriteAsync(payload, cancellationToken)
                            .ConfigureAwait(false);
                }
                else
                {
                    var frameSize = ZlinkStreamFrameCodec.GetFrameSize(
                        header.Length,
                        payload.Length
                    );
                    var frame = ArrayPool<byte>.Shared.Rent(frameSize);
                    try
                    {
                        ZlinkStreamFrameCodec.WriteFrame(
                            frame.AsSpan(0, frameSize),
                            header,
                            payload
                        );
                        await connection
                            .WriteAsync(frame.AsMemory(0, frameSize), cancellationToken)
                            .ConfigureAwait(false);
                    }
                    finally
                    {
                        ArrayPool<byte>.Shared.Return(frame);
                    }
                }
            }
            finally
            {
                sendGate.Release();
            }
        }
        catch (Exception ex) when (ex is not ZlinkStreamException)
        {
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.SendFailed, "Send failed.", ex);
        }
    }

    /// <summary>
    ///     Decompresses a received payload. A payload the codec cannot decompress is
    ///     DecompressionFailed; a result over the receive limit is FrameTooLarge, the code of
    ///     a received payload over the limit (stream-connector spec §4.7, §9).
    /// </summary>
    public ReadOnlyMemory<byte> DecompressIfNeeded(
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload
    )
    {
        if (!header.Flags.HasFlag(ZlinkStreamHeaderFlags.PayloadCompressed))
            return payload;

        if (options.Compression == ZlinkStreamCompression.None || compressionCodec is null)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.DecompressionFailed,
                "Compression codec is not configured."
            );

        ReadOnlyMemory<byte> decompressed;
        try
        {
            decompressed = compressionCodec.Decompress(payload, options.MaxReceivePayloadSize);
        }
        catch (Exception ex) when (ex is not ZlinkStreamException)
        {
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.DecompressionFailed,
                "Decompression failed.",
                ex
            );
        }

        // A custom codec may not apply the limit it is given, so the result is checked here.
        if (decompressed.Length > options.MaxReceivePayloadSize)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameTooLarge,
                "Decompressed payload exceeds MaxReceivePayloadSize."
            );

        return decompressed;
    }

    private ReadOnlyMemory<byte> CompressPayload(ReadOnlyMemory<byte> payload)
    {
        if (options.Compression == ZlinkStreamCompression.None || compressionCodec is null)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.CompressionFailed,
                "Compression codec is not configured."
            );

        try
        {
            return compressionCodec.Compress(payload);
        }
        catch (Exception ex)
        {
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.CompressionFailed,
                "Compression failed.",
                ex
            );
        }
    }
}
