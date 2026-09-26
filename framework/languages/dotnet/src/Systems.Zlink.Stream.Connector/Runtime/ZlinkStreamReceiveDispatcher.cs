using System.Text.Json;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamReceiveDispatcher(
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
        var header = headerCodec.Decode(frame.Header, captureFlow: false);
        if (header.Kind == ZlinkStreamMessageKind.Control)
        {
            await DispatchControlAsync(header, frame.Payload, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (header.Kind == ZlinkStreamMessageKind.Response)
        {
            var request = pending.TakeReply(header);
            if (request is null)
                return;

            try
            {
                var decoded = frameSender.DecompressIfNeeded(header, frame.Payload);
                request.Complete(
                    new ZlinkStreamPendingCompletion(header, frame with { Payload = decoded }, null)
                );
            }
            catch (ZlinkStreamException failure)
            {
                request.Complete(
                    new ZlinkStreamPendingCompletion(
                        header,
                        frame,
                        failure.Error.Code == ZlinkStreamErrorCode.DecompressionFailed
                            ? failure.Error
                            : new ZlinkStreamError(
                                ZlinkStreamErrorCode.Disconnected,
                                "Connection ended before the reply completed."
                            )
                    )
                );
                if (failure.Error.Code != ZlinkStreamErrorCode.DecompressionFailed)
                    throw;
            }
            return;
        }

        var actor = header.ActorSlot is { } actorSlot ? actors.Resolve(actorSlot) : null;

        // The receive path decompresses every payload here, once. A payload that does not
        // decompress fails only its packet - the pending request it answers, or else the
        // error event - and the connection stays (stream-connector spec §9). A result over
        // the receive limit is FrameTooLarge and ends the connection in the receive loop.
        ReadOnlyMemory<byte> payload;
        try
        {
            payload = frameSender.DecompressIfNeeded(header, frame.Payload);
        }
        catch (ZlinkStreamException failure)
            when (failure.Error.Code == ZlinkStreamErrorCode.DecompressionFailed)
        {
            var request = pending.TakeReply(header);
            if (request is not null)
                request.Complete(new ZlinkStreamPendingCompletion(header, frame, failure.Error));
            else
                await callbacks
                    .PublishErrorAsync(failure.Error, cancellationToken)
                    .ConfigureAwait(false);
            return;
        }

        frame = frame with { Payload = payload };
        if (header.Kind == ZlinkStreamMessageKind.Error)
        {
            var error = ParseErrorPayload(frame.Payload);
            var request = pending.TakeReply(header);
            if (request is not null)
                request.Complete(new ZlinkStreamPendingCompletion(header, frame, error));
            else
                await callbacks.PublishErrorAsync(error, cancellationToken).ConfigureAwait(false);
            return;
        }

        await DispatchTypedHandlersAsync(header, frame.Payload, actor, cancellationToken)
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
        ReadOnlyMemory<byte> payload,
        ZlinkStreamActor? actor,
        CancellationToken cancellationToken
    )
    {
        var payloadObject = new ZlinkStreamEncodedPayload(header.Codec, payload);
        var message = new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(
            header.Name,
            header.Metadata,
            payloadObject,
            actor?.ActorId
        );

        // The packet enters the receive queue and is counted in the same step that hands it
        // to the dispatch mode, so a counted message is already observable to Dispatch and
        // WaitFor (spec §10). Its handlers - the connector's and, for a packet of an Actor,
        // that Actor handle's - are decided when it is dispatched (spec §5.6, §7).
        var entry = new MessageEntry(typedHandlers, actor, receivedMessages, callbacks);
        await callbacks
            .DispatchEntriesAsync(
                [entry],
                cancellationToken,
                () => entry.Node = receivedMessages.Record(message)
            )
            .ConfigureAwait(false);
    }

    /// <summary>
    ///     A received packet in the dispatch queue. It goes to the handlers registered when
    ///     it is dispatched; with none it stays queued, and a wait may take it meanwhile.
    /// </summary>
    private sealed class MessageEntry(
        ZlinkStreamTypedHandlerRegistry handlers,
        ZlinkStreamActor? actor,
        ZlinkStreamReceivedMessages receivedMessages,
        ZlinkStreamConnectorCallbacks callbacks
    ) : ZlinkStreamDispatchEntry
    {
        public LinkedListNode<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>? Node { get; set; }

        public override bool ReportErrors => false;

        public override int PendingCallbacks =>
            Node is { } node && receivedMessages.IsUnread(node)
                ? Registered(node.Value.Name).Count
                : 0;

        public override Func<CancellationToken, ValueTask>? Take(out bool keep)
        {
            var node = Node!;
            var registered = Registered(node.Value.Name);
            if (registered.Count == 0)
            {
                keep = receivedMessages.IsUnread(node);
                return null;
            }

            keep = false;
            if (!receivedMessages.TryTake(node))
                return null;

            var message = node.Value;
            return async token =>
            {
                foreach (var handler in registered)
                    await callbacks
                        .InvokeUserCallbackInlineAsync(t => handler.Invoke(message, t), token)
                        .ConfigureAwait(false);
            };
        }

        /// <summary>The connector handlers, then the Actor handle handlers, registered now.</summary>
        private IReadOnlyList<ZlinkStreamTypedHandlerRegistry.TypedHandler> Registered(string name)
        {
            var connectorHandlers = handlers.Snapshot(name);
            if (actor is null)
                return connectorHandlers;
            var actorHandlers = actor.Handlers(name);
            if (actorHandlers.Count == 0)
                return connectorHandlers;
            return [.. connectorHandlers, .. actorHandlers];
        }
    }

    private static ZlinkStreamError ParseErrorPayload(ReadOnlyMemory<byte> payload)
    {
        try
        {
            var dto = JsonSerializer.Deserialize<WireError>(payload.Span, JsonOptions);
            if (dto is null || string.IsNullOrWhiteSpace(dto.Code) || dto.Message is null)
                throw new JsonException("Remote stream error code and message are required.");
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
