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
    private const int CorrelationIdCharacterCount = sizeof(long) * 2;
    private const string CorrelationIdFormat = "x";

    private static readonly ReadOnlyMemory<byte> HeartbeatPing = EncodeHeartbeat(
        ZlinkStreamConnector.HeartbeatPingName
    );
    private static readonly ReadOnlyMemory<byte> HeartbeatPong = EncodeHeartbeat(
        ZlinkStreamConnector.HeartbeatPongName
    );

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
        Span<char> correlationId = stackalloc char[CorrelationIdCharacterCount];
        var correlationLength = 0;
        if (kind == ZlinkStreamMessageKind.Request)
            // Every Int64, including counter wraparound, fits in 16 hex digits.
            ZlinkStreamCorrelation
                .NextValue()
                .TryFormat(correlationId, out correlationLength, CorrelationIdFormat);

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

    private static byte[] EncodeHeartbeat(string name)
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            name,
            ZlinkStreamMetadata.Empty,
            null,
            null,
            null,
            null
        );
        return ZlinkStreamFrameCodec.Encode(
            new ZlinkStreamHeaderCodec().Encode(header).Span,
            ReadOnlySpan<byte>.Empty
        );
    }

    public async ValueTask SendHeartbeatAsync(bool pong, CancellationToken cancellationToken)
    {
        var connection =
            connectionProvider()
            ?? throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.Disconnected,
                "Connector is not connected."
            );
        await SendPacketAsync(
                connection,
                default,
                default,
                cancellationToken,
                pong ? HeartbeatPong : HeartbeatPing
            )
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
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> encodedFrame = default
    )
    {
        try
        {
            await sendGate.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                if (!encodedFrame.IsEmpty)
                {
                    await connection
                        .WriteAsync(encodedFrame, cancellationToken)
                        .ConfigureAwait(false);
                    return;
                }

                // One write per frame, as the Java, C++ and Node connectors do. A frame split
                // across writes leaves its tail behind Nagle until the peer's delayed ACK (#1382).
                var frameSize = ZlinkStreamFrameCodec.GetFrameSize(header.Length, payload.Length);
                var frame = ArrayPool<byte>.Shared.Rent(frameSize);
                try
                {
                    ZlinkStreamFrameCodec.WriteFrame(frame.AsSpan(0, frameSize), header, payload);
                    await connection
                        .WriteAsync(frame.AsMemory(0, frameSize), cancellationToken)
                        .ConfigureAwait(false);
                }
                finally
                {
                    ArrayPool<byte>.Shared.Return(frame);
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
