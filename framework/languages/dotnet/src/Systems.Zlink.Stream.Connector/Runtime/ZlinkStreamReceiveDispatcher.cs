using System.Text.Json;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamReceiveDispatcher(
    ZlinkStreamHeaderCodec headerCodec,
    ZlinkStreamPendingRequests pending,
    ZlinkStreamTypedHandlerRegistry typedHandlers,
    ZlinkStreamReceivedMessages receivedMessages,
    ZlinkStreamFrameSender frameSender,
    ZlinkStreamConnectorCallbacks callbacks,
    ZlinkStreamInboundObserverDispatcher inboundObservers,
    Func<ZlinkStreamCloseReason, string?, CancellationToken, ValueTask> closeFromServer)
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public async ValueTask DispatchPacketAsync(ZlinkStreamFrame frame, CancellationToken cancellationToken)
    {
        var header = headerCodec.Decode(frame.Header);
        inboundObservers.Enqueue(header, frame.Payload);

        if (header.Kind == ZlinkStreamMessageKind.Control)
        {
            await DispatchControlAsync(header, frame.Payload, cancellationToken).ConfigureAwait(false);
            return;
        }

        if (pending.TryComplete(header, frame, ParseErrorPayload)) return;

        if (header.Kind == ZlinkStreamMessageKind.Response) return;

        if (header.Kind == ZlinkStreamMessageKind.Error)
        {
            await callbacks.PublishErrorAsync(ParseErrorPayload(frame.Payload), cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await DispatchTypedHandlersAsync(header, frame.Payload, cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask DispatchControlAsync(
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        if (header.Name == ZlinkStreamSessionClosingCodec.ControlName)
        {
            var closing = ZlinkStreamSessionClosingCodec.Decode(payload.Span);
            await closeFromServer(closing.Reason, closing.Diagnostic, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (payload.Length != 0)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Heartbeat control packet payload must be empty.");

        if (header.Name == ZlinkStreamConnector.HeartbeatPingName)
        {
            await frameSender.SendControlAsync(ZlinkStreamConnector.HeartbeatPongName, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (header.Name == ZlinkStreamConnector.HeartbeatPongName) return;

        throw ZlinkStreamConnector.Error(
            ZlinkStreamErrorCode.FrameDecodeFailed,
            "Unknown control packet.");
    }

    private async ValueTask DispatchTypedHandlersAsync(
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> wirePayload,
        CancellationToken cancellationToken)
    {
        var payload = frameSender.DecompressIfNeeded(header, wirePayload);
        var payloadObject = new ZlinkStreamEncodedPayload(header.Codec, payload);
        var message = new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(header.Name, header.Metadata, payloadObject);
        var handlers = typedHandlers.Snapshot(header.Name);
        if (handlers.Count == 0 && !receivedMessages.Record(message))
        {
            await callbacks.PublishErrorAsync(
                    new ZlinkStreamError(
                        ZlinkStreamErrorCode.ReceivedMessageDropped,
                        "Received stream message was dropped because the received-message queue is full."),
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        foreach (var handler in handlers)
            await callbacks.DispatchUserCallbackAsync(
                    async dispatchedToken =>
                    {
                        using var flow = ZlinkStreamFlowContext.Enter(header.FlowId, header.FlowOrigin);
                        await handler.Invoke(message, dispatchedToken).ConfigureAwait(false);
                    },
                    cancellationToken)
                .ConfigureAwait(false);
    }

    private static ZlinkStreamError ParseErrorPayload(ReadOnlyMemory<byte> payload)
    {
        try
        {
            var dto = JsonSerializer.Deserialize<WireError>(payload.Span, JsonOptions);
            if (dto is null || string.IsNullOrWhiteSpace(dto.Code))
                throw new JsonException("Remote stream error code is required.");
            return new ZlinkStreamError(
                ZlinkStreamErrorCode.RemoteError,
                string.IsNullOrWhiteSpace(dto.Message)
                    ? dto.Code
                    : $"{dto.Code}: {dto.Message}");
        }
        catch (Exception ex)
        {
            return new ZlinkStreamError(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Remote stream error payload could not be decoded.",
                ex);
        }
    }

    private sealed record WireError(string? Code, string? Message);
}
