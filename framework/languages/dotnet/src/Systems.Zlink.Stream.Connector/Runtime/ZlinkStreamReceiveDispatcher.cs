using System.Text.Json;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamReceiveDispatcher(
    ZlinkStreamConnectorOptions options,
    ZlinkStreamHeaderCodec headerCodec,
    ZlinkStreamPendingRequests pending,
    ZlinkStreamTypedHandlerRegistry typedHandlers,
    ZlinkStreamReceivedMessages receivedMessages,
    ZlinkStreamFrameSender frameSender,
    ZlinkStreamConnectorCallbacks callbacks,
    ZlinkStreamActors actors,
    Func<ZlinkStreamCloseReason, string?, CancellationToken, ValueTask> closeFromServer
)
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public async ValueTask DispatchPacketAsync(
        ZlinkStreamFrame frame,
        CancellationToken cancellationToken
    )
    {
        // Read the diagnostics level exactly once for this packet's processing so a
        // concurrent level change never splits header decode from flow-scope
        // installation within the same dispatch.
        var diagnosticsLevel = options.DiagnosticsLevel;

        // At Off, inbound flow fields are framing only: keep the structural length
        // checks but skip validation, allocation, and flow-context installation.
        var header = headerCodec.Decode(
            frame.Header,
            diagnosticsLevel != ZlinkStreamDiagnosticsLevel.Off
        );
        if (header.Kind == ZlinkStreamMessageKind.Control)
        {
            await DispatchControlAsync(header, frame.Payload, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        var actor = header.ActorSlot is { } actorSlot ? actors.Resolve(actorSlot) : null;

        if (pending.TryComplete(header, frame, ParseErrorPayload))
            return;

        if (header.Kind == ZlinkStreamMessageKind.Response)
            return;

        if (header.Kind == ZlinkStreamMessageKind.Error)
        {
            await callbacks
                .PublishErrorAsync(ParseErrorPayload(frame.Payload), cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await DispatchTypedHandlersAsync(
                header,
                frame.Payload,
                actor,
                diagnosticsLevel,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    private async ValueTask DispatchControlAsync(
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        if (header.Name == ZlinkStreamSessionClosingCodec.ControlName)
        {
            var closing = ZlinkStreamSessionClosingCodec.Decode(payload.Span);
            await closeFromServer(closing.Reason, closing.Diagnostic, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (
            header.Name
            is ZlinkStreamActors.BoundControlName
                or ZlinkStreamActors.UnboundControlName
        )
        {
            await actors
                .DispatchControlAsync(header.Name, payload, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (payload.Length != 0)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Heartbeat control packet payload must be empty."
            );

        if (header.Name == ZlinkStreamConnector.HeartbeatPingName)
        {
            await frameSender
                .SendControlAsync(ZlinkStreamConnector.HeartbeatPongName, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (header.Name == ZlinkStreamConnector.HeartbeatPongName)
            return;

        throw ZlinkStreamConnector.Error(
            ZlinkStreamErrorCode.FrameDecodeFailed,
            "Unknown control packet."
        );
    }

    private async ValueTask DispatchTypedHandlersAsync(
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> wirePayload,
        ZlinkStreamActor? actor,
        ZlinkStreamDiagnosticsLevel diagnosticsLevel,
        CancellationToken cancellationToken
    )
    {
        var payload = frameSender.DecompressIfNeeded(header, wirePayload);
        var payloadObject = new ZlinkStreamEncodedPayload(header.Codec, payload);
        // The flow pair travels with the message so application code can align its own
        // logs with the server trace (stream-connector spec §5.5). At Off the header
        // carries no captured flow, so both values stay null.
        var message = new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(
            header.Name,
            header.Metadata,
            payloadObject,
            header.FlowId,
            header.FlowOrigin,
            actor?.ActorId
        );

        // Counted on arrival, before any surface takes it: the value must not depend on
        // whether a handler is registered or on the dispatch mode (spec §10).
        receivedMessages.CountArrival(header.Name);
        var handlers = typedHandlers.Snapshot(header.Name);
        if (handlers.Count == 0)
            receivedMessages.Record(message);

        foreach (var handler in handlers)
            await callbacks
                .DispatchUserCallbackAsync(
                    async dispatchedToken =>
                    {
                        using var flow =
                            diagnosticsLevel == ZlinkStreamDiagnosticsLevel.Off
                                ? null
                                : ZlinkStreamFlowContext.Enter(header.FlowId, header.FlowOrigin);
                        await handler.Invoke(message, dispatchedToken).ConfigureAwait(false);
                    },
                    cancellationToken
                )
                .ConfigureAwait(false);

        if (actor is not null)
            await callbacks
                .DispatchUserCallbackAsync(
                    async dispatchedToken =>
                    {
                        foreach (var handler in actor.Handlers(header.Name))
                            await callbacks
                                .InvokeUserCallbackInlineAsync(
                                    async handlerToken =>
                                    {
                                        using var flow =
                                            diagnosticsLevel == ZlinkStreamDiagnosticsLevel.Off
                                                ? null
                                                : ZlinkStreamFlowContext.Enter(
                                                    header.FlowId,
                                                    header.FlowOrigin
                                                );
                                        await handler
                                            .Invoke(message, handlerToken)
                                            .ConfigureAwait(false);
                                    },
                                    dispatchedToken
                                )
                                .ConfigureAwait(false);
                    },
                    cancellationToken,
                    reportErrors: false
                )
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
                string.IsNullOrWhiteSpace(dto.Message) ? dto.Code : $"{dto.Code}: {dto.Message}"
            );
        }
        catch (Exception ex)
        {
            return new ZlinkStreamError(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Remote stream error payload could not be decoded.",
                ex
            );
        }
    }

    private sealed record WireError(string? Code, string? Message);
}
