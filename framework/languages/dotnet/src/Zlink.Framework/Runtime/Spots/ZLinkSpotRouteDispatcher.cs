using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotRouteDispatcher(
    string channelName,
    string spotId,
    ZLinkSpotPacketRegistry packets,
    Func<ZLinkSpotHandlerInvoker> handlerInvoker,
    ZLinkCodecRegistryBuilder codecs,
    ZLinkDispatchErrorReporter dispatchErrors,
    Func<ZLinkBackendRouteReceived, ZLinkEnvelopeHeader, CancellationToken, ValueTask<bool>>? internalPackets = null)
{
    public async ValueTask DispatchAsync(
        ZLinkBackendRouteReceived received,
        CancellationToken cancellationToken)
    {
        using var applicationAdmission =
            received.ApplicationJobAdmission is { } admission
                ? ZLinkApplicationJobQueueInvocation.Enter(admission)
                : null;
        using (received)
        {
        if (received.Parts.Count == 0)
        {
            HandleProtocolError(received, ZLinkEnvelopeCodec.MissingHeader());
            return;
        }

            ZLinkEnvelopeHeader header;
            try
            {
            header = ZLinkEnvelopeCodec.DecodeHeader(
                received.Parts,
                dispatchErrors.Flow.CaptureEnabled);
            ZLinkEnvelopeCodec.ValidateDispatchHeader(header);
            }
            catch (ZLinkEnvelopeProtocolException protocolError)
            {
                HandleProtocolError(received, protocolError);
                return;
            }
            using var currentFlow = ZLinkFlowContext.Enter(
                header.FlowId,
                header.FlowOrigin,
                dispatchErrors.Flow.CaptureEnabled,
                ZLinkFlowOrigin.Inbound);
            var kind = header.Kind == ZLinkMessageKind.Request
                ? ZLinkDispatchMessageKind.Request
                : ZLinkDispatchMessageKind.Send;
            var scope = CreateScope(header, kind);

            scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Received);

            if (internalPackets is not null
                && await internalPackets(received, header, cancellationToken).ConfigureAwait(false))
                return;

            if (!packets.TryResolve(header, out var descriptor) || descriptor is null)
            {
                if (header.Kind == ZLinkMessageKind.Request)
                {
                    // Route resolution failure is framework-generated
                    // (zlink.origin marker on the resulting error reply); it
                    // must stay distinguishable from an application handler's
                    // own NotFound.
                    var error = new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"No SPOT route request handler is registered for '{channelName}:{header.MessageName}'.")
                    {
                        Origin = ZLinkErrorOrigin.Framework
                    };
                    scope.HandlerMissing(
                        dispatchErrors,
                        ZLinkDispatchErrorAction.ReplyError,
                        error);
                    await ReplyErrorAsync(
                            received, header, error,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
                else
                {
                    scope.Dropped(dispatchErrors);
                }

                return;
            }

            object? message;
            if (descriptor.IsRequest)
            {
                if (!scope.TryDecode(
                        received.Parts,
                        descriptor.MessageType,
                        header.ContentType,
                        codecs,
                        dispatchErrors,
                        ZLinkDispatchErrorAction.ReplyError,
                        "SPOT route request",
                        out message,
                        out var decodeError))
                {
                    await ReplyErrorAsync(
                            received, header, decodeError!,
                            cancellationToken)
                        .ConfigureAwait(false);
                    return;
                }
            }
            else
            {
                if (!scope.TryDecode(
                        received.Parts,
                        descriptor.MessageType,
                        header.ContentType,
                        codecs,
                        dispatchErrors,
                        ZLinkDispatchErrorAction.Drop,
                        out message))
                    return;
            }

            if (!descriptor.IsRequest)
            {
                try
                {
                    await handlerInvoker()
                        .InvokePacketAsync(descriptor, message, cancellationToken)
                        .ConfigureAwait(false);

                    scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Dispatched);
                }
                catch (Exception ex)
                {
                    scope.HandlerException(
                        dispatchErrors,
                        ZLinkDispatchErrorAction.Drop,
                        ex);
                }

                return;
            }

            IReadOnlyList<Message> replyParts;
            try
            {
                var reply = await handlerInvoker()
                    .InvokeRequestAsync(descriptor, message, cancellationToken)
                    .ConfigureAwait(false);
                replyParts = ZLinkSpotReplyEnvelope.EncodeResponseParts(
                    channelName,
                    descriptor.MessageName,
                    header.CorrelationId,
                    reply,
                    descriptor.ReplyType,
                    codecs);

                scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Replied);
            }
            catch (Exception ex)
            {
                replyParts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
                    channelName,
                    descriptor.MessageName,
                    header.CorrelationId,
                    ex);
                scope.HandlerException(
                    dispatchErrors,
                    ZLinkDispatchErrorAction.ReplyError,
                    ex);
            }

            await SubmitReplyAsync(
                    received, replyParts, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private ZLinkDispatchFlowScope CreateScope(
        ZLinkEnvelopeHeader header,
        ZLinkDispatchMessageKind kind)
    {
        return new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotRoute,
            dispatchErrors.Flow.CaptureEnabled,
            kind,
            header.MessageName,
            channelName,
            header.ContentType,
            header.CorrelationId,
            spotId: spotId);
    }

    private ValueTask SubmitReplyAsync(
        ZLinkBackendRouteReceived received,
        IReadOnlyList<Message> replyParts,
        CancellationToken cancellationToken)
    {
        return ZLinkSpotReplySubmitter.SubmitDirectAsync(
            received, replyParts, cancellationToken);
    }

    private ValueTask ReplyErrorAsync(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader header,
        Exception exception,
        CancellationToken cancellationToken)
    {
        var replyParts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
            channelName, header.MessageName, header.CorrelationId, exception);
        return SubmitReplyAsync(
            received, replyParts, cancellationToken);
    }

    private void ReplyError(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader header,
        Exception exception)
    {
        var replyParts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
            channelName,
            header.MessageName,
            header.CorrelationId,
            exception);
        ZLinkSpotReplySubmitter.SubmitAndDispose(received, replyParts);
    }

    private void HandleProtocolError(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeProtocolException protocolError)
    {
        var header = protocolError.Header;
        var isRequest = received.RequestSeq.HasValue;
        var canReply = isRequest && ZLinkEnvelopeCodec.CanCorrelateReply(header);
        if (dispatchErrors.Enabled)
        {
            // Keep whatever flow the invalid frame carried in readable form,
            // but never fabricate a fresh id for its failure record (spec 27 §7).
            var validFlow = ZLinkEnvelopeCodec.ValidFlow(header);
            using var flow = ZLinkFlowContext.Enter(
                validFlow.FlowId,
                validFlow.FlowOrigin,
                dispatchErrors.Flow.CaptureEnabled,
                ZLinkFlowOrigin.Inbound,
                createIfAbsent: false);
            dispatchErrors.Report(new ZLinkDispatchFailure(
                ZLinkDispatchErrorSurface.SpotRoute,
                isRequest
                    ? ZLinkDispatchMessageKind.Request
                    : ZLinkDispatchMessageKind.Send,
                ZLinkDispatchErrorReason.InvalidFrame,
                canReply
                    ? ZLinkDispatchErrorAction.ReplyError
                    : ZLinkDispatchErrorAction.Drop,
                header.MessageName,
                channelName,
                SpotId: spotId,
                CorrelationId: header.CorrelationId,
                Exception: protocolError));
        }
        if (!canReply) return;

        var replyParts = ZLinkSpotReplyEnvelope.EncodeProtocolErrorParts(
            channelName,
            header,
            protocolError.Message);
        ZLinkSpotReplySubmitter.SubmitAndDispose(received, replyParts);
    }
}
