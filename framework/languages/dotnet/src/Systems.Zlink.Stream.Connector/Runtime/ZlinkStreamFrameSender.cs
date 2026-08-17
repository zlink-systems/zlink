using System.Buffers;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamFrameSender(
    ZlinkStreamConnectorOptions options,
    ZlinkStreamHeaderCodec headerCodec,
    IZlinkStreamCompressionCodec? compressionCodec,
    SemaphoreSlim sendGate,
    Func<IZlinkStreamConnection?> connectionProvider)
{
    public ZlinkStreamOutboundFrame BuildOutboundFrame(
        ZlinkStreamMessageKind kind,
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        ZlinkStreamRequestSeq? requestSeq)
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

        // Correlation ids are protocol information linking a request with its terminal
        // reply, so requests carry one at every diagnostics level. One-way sends have
        // no reply and therefore never carry a correlation id (flow-correlation spec §2).
        var correlationId = kind == ZlinkStreamMessageKind.Request
            ? ZlinkStreamCorrelation.Next()
            : null;

        // Flow fields are trace-only: at Off nothing is created or attached and the
        // ambient flow context is not even read.
        string? flowId = null;
        ZlinkStreamFlowOrigin? flowOrigin = null;
        if (kind != ZlinkStreamMessageKind.Control
            && options.DiagnosticsLevel != ZlinkStreamDiagnosticsLevel.Off)
        {
            var flow = ZlinkStreamFlowContext.Current;
            flowId = flow?.FlowId ?? ZlinkStreamFlowId.Create();
            flowOrigin = flow?.Origin ?? ZlinkStreamFlowOrigin.Application;
        }

        var header = new ZlinkStreamHeader(
            kind,
            payload.Codec,
            flags,
            requestSeq,
            name,
            metadata,
            correlationId,
            flowId,
            flowOrigin);
        return new ZlinkStreamOutboundFrame(EncodeHeaderForSend(header), payloadBytes);
    }

    public async ValueTask SendControlAsync(string name, CancellationToken cancellationToken)
    {
        var frame = BuildOutboundFrame(
            ZlinkStreamMessageKind.Control,
            name,
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, ReadOnlyMemory<byte>.Empty),
            ZlinkStreamMetadata.Empty,
            false,
            null);
        ValidateSendReady(frame.HeaderBytes, frame.PayloadBytes);
        await SendPacketAsync(frame.HeaderBytes, frame.PayloadBytes, cancellationToken).ConfigureAwait(false);
    }

    public void ValidateSendReady(ReadOnlyMemory<byte> header, ReadOnlyMemory<byte> payload)
    {
        ZlinkStreamFrameCodec.ValidateSendFrame(header.Length, payload.Length);
        if (connectionProvider() is null)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.Disconnected, "Connector is not connected.");
    }

    public async ValueTask SendPacketAsync(
        ReadOnlyMemory<byte> header,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        var connection = connectionProvider();
        if (connection is null)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.Disconnected, "Connector is not connected.");

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
                            payload.Length);
                        await connection.WriteAsync(
                            prefix.AsMemory(0, 6),
                            cancellationToken).ConfigureAwait(false);
                    }
                    finally
                    {
                        ArrayPool<byte>.Shared.Return(prefix);
                    }

                    if (header.Length > 0) await connection.WriteAsync(header, cancellationToken).ConfigureAwait(false);

                    if (payload.Length > 0)
                        await connection.WriteAsync(payload, cancellationToken).ConfigureAwait(false);
                }
                else
                {
                    var frameSize = ZlinkStreamFrameCodec.GetFrameSize(
                        header.Length,
                        payload.Length);
                    var frame = ArrayPool<byte>.Shared.Rent(frameSize);
                    try
                    {
                        ZlinkStreamFrameCodec.WriteFrame(
                            frame.AsSpan(0, frameSize),
                            header,
                            payload);
                        await connection.WriteAsync(
                            frame.AsMemory(0, frameSize),
                            cancellationToken).ConfigureAwait(false);
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

    public ReadOnlyMemory<byte> DecompressIfNeeded(ZlinkStreamHeader header, ReadOnlyMemory<byte> payload)
    {
        if (!header.Flags.HasFlag(ZlinkStreamHeaderFlags.PayloadCompressed)) return payload;

        if (options.Compression == ZlinkStreamCompression.None || compressionCodec is null)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.DecompressionFailed,
                "Compression codec is not configured.");

        try
        {
            return compressionCodec.Decompress(payload, int.MaxValue);
        }
        catch (Exception ex)
        {
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.DecompressionFailed, "Decompression failed.", ex);
        }
    }

    private ReadOnlyMemory<byte> EncodeHeaderForSend(ZlinkStreamHeader header)
    {
        return headerCodec.Encode(header);
    }

    private ReadOnlyMemory<byte> CompressPayload(ReadOnlyMemory<byte> payload)
    {
        if (options.Compression == ZlinkStreamCompression.None || compressionCodec is null)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.CompressionFailed,
                "Compression codec is not configured.");

        try
        {
            return compressionCodec.Compress(payload);
        }
        catch (Exception ex)
        {
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.CompressionFailed, "Compression failed.", ex);
        }
    }
}
