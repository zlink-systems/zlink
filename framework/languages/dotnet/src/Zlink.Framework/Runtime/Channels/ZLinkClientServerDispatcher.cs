namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkClientServerDispatcher(
    ZLinkChannelCommandDispatchPipeline commandPipeline,
    ZLinkChannelRequestDispatchPipeline requestPipeline,
    ZLinkCodecRegistryBuilder codecs,
    Func<bool> flowCaptureEnabled)
{
    public async ValueTask DispatchAsync(
        string channelName,
        IRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate,
        uint maximumMessageBytes,
        CancellationToken cancellationToken)
    {
        ZLinkEnvelopeHeader header;
        try
        {
            header = ZLinkEnvelopeCodec.DecodeHeader(
                received.Parts,
                flowCaptureEnabled());
            if (!StringComparer.Ordinal.Equals(header.ChannelName, channelName))
                throw new ZLinkEnvelopeProtocolException(
                    header,
                    $"ClientServer channel '{channelName}' received an envelope for "
                    + $"'{header.ChannelName}'.");
        }
        catch (ZLinkEnvelopeProtocolException protocolError)
        {
            ReplyProtocolError(
                channelName,
                router,
                received,
                replyGate,
                protocolError.Header,
                protocolError.Message,
                maximumMessageBytes);
            return;
        }

        // Spec 27 §5: the ClientServer handler context preserves the inbound
        // flow pair, so downstream calls reuse it and the reply encoder finds
        // it in the ambient context. Off suppresses installation entirely (§4).
        using var currentFlow = ZLinkFlowContext.Enter(
            header.FlowId,
            header.FlowOrigin,
            flowCaptureEnabled(),
            ZLinkFlowOrigin.Inbound);
        switch (header.Kind)
        {
            case ZLinkMessageKind.Command:
                await commandPipeline.DispatchAsync(
                        channelName,
                        received.Parts,
                        header,
                        cancellationToken)
                    .ConfigureAwait(false);
                break;
            case ZLinkMessageKind.Request:
                await requestPipeline.DispatchAsync(
                        channelName,
                        received.Parts,
                        header,
                        (Self: this, replyGate, router, received, header, maximumMessageBytes),
                        static (s, replyHeader, reply, replyType) =>
                        {
                            s.Self.Reply(
                                s.replyGate,
                                s.router,
                                s.received,
                                s.header,
                                replyHeader,
                                reply,
                                replyType,
                                s.maximumMessageBytes);
                            return ValueTask.CompletedTask;
                        },
                        static (s, errorHeader) =>
                        {
                            s.Self.Reply(
                                s.replyGate,
                                s.router,
                                s.received,
                                s.header,
                                errorHeader,
                                null,
                                null,
                                s.maximumMessageBytes);
                            return ValueTask.CompletedTask;
                        },
                        cancellationToken)
                    .ConfigureAwait(false);
                break;
            default:
                ReplyProtocolError(
                    channelName,
                    router,
                    received,
                    replyGate,
                    header,
                    $"ClientServer server cannot accept '{header.Kind}' envelopes.",
                    maximumMessageBytes);
                break;
        }
    }

    private void ReplyProtocolError(
        string channelName,
        IRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate,
        ZLinkEnvelopeHeader request,
        string message,
        uint maximumMessageBytes)
    {
        if (!ZLinkEnvelopeCodec.CanCorrelateReply(request))
            return;
        Reply(
            replyGate,
            router,
            received,
            request,
            ZLinkChannelReplyWriter.CreateProtocolErrorHeader(
                channelName,
                request,
                message),
            null,
            null,
            maximumMessageBytes);
    }

    internal void RejectMessageTooLarge(
        string channelName,
        IRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate,
        uint maximumMessageBytes)
    {
        try
        {
            var request = ZLinkEnvelopeCodec.DecodeHeader(
                received.Parts,
                flowCaptureEnabled());
            if (request.Kind != ZLinkMessageKind.Request
                || !StringComparer.Ordinal.Equals(request.ChannelName, channelName))
                return;
            Reply(
                replyGate,
                router,
                received,
                request,
                ZLinkChannelReplyWriter.CreateErrorHeader(
                    channelName,
                    request,
                    ZLinkClientServerMessageBound.CreateExceededException(
                        maximumMessageBytes)),
                null,
                null,
                maximumMessageBytes);
        }
        catch (ZLinkEnvelopeProtocolException)
        {
        }
    }

    private void Reply(
        ZLinkChannelReplyGate replyGate,
        IRouterSocket router,
        Received received,
        ZLinkEnvelopeHeader requestHeader,
        ZLinkEnvelopeHeader replyHeader,
        object? body,
        Type? bodyType,
        uint maximumMessageBytes)
    {
        replyGate.TryInvoke(() =>
        {
            if (received.RoutingId is not { } sourceRid
                || received.RequestSeq is not { } requestSeq)
                return;

            var reply = ZLinkEnvelopeCodec.EncodeParts(
                replyHeader,
                body,
                bodyType,
                codecs);
            if (!ZLinkClientServerMessageBound.Fits(
                    reply,
                    maximumMessageBytes))
            {
                ZLinkMessageParts.DisposeAll(reply);
                reply = ZLinkEnvelopeCodec.EncodeParts(
                    ZLinkChannelReplyWriter.CreateErrorHeader(
                        requestHeader.ChannelName,
                        requestHeader,
                        ZLinkClientServerMessageBound.CreateExceededException(
                            maximumMessageBytes)),
                    null,
                    null,
                    codecs);
                if (!ZLinkClientServerMessageBound.Fits(
                        reply,
                        maximumMessageBytes))
                {
                    ZLinkMessageParts.DisposeAll(reply);
                    return;
                }
            }
            try
            {
                router.Reply(sourceRid, requestSeq)
                    .Messages(reply)
                    .Submit();
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(reply);
            }
        });
    }
}
