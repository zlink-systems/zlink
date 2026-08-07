namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkClientServerDispatcher(
    ZLinkChannelCommandDispatchPipeline commandPipeline,
    ZLinkChannelRequestDispatchPipeline requestPipeline,
    ZLinkCodecRegistryBuilder codecs)
{
    public async ValueTask DispatchAsync(
        string channelName,
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate,
        uint maximumMessageBytes,
        CancellationToken cancellationToken)
    {
        ZLinkEnvelopeHeader header;
        try
        {
            header = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
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
                        (replyHeader, reply, replyType) =>
                        {
                            Reply(
                                replyGate,
                                router,
                                received,
                                header,
                                replyHeader,
                                reply,
                                replyType,
                                maximumMessageBytes);
                            return ValueTask.CompletedTask;
                        },
                        errorHeader =>
                        {
                            Reply(
                                replyGate,
                                router,
                                received,
                                header,
                                errorHeader,
                                null,
                                null,
                                maximumMessageBytes);
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

    internal void RejectOverloaded(
        string channelName,
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate,
        uint maximumMessageBytes)
    {
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
            if (header.Kind != ZLinkMessageKind.Request
                || !StringComparer.Ordinal.Equals(header.ChannelName, channelName))
                return;
            Reply(
                replyGate,
                router,
                received,
                header,
                ZLinkChannelReplyWriter.CreateErrorHeader(
                    channelName,
                    header,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Rejected,
                        $"ClientServer channel '{channelName}' application queue is full.",
                        retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff)),
                null,
                null,
                maximumMessageBytes);
        }
        catch (ZLinkEnvelopeProtocolException)
        {
        }
    }

    private void ReplyProtocolError(
        string channelName,
        IZLinkBackendRouterSocket router,
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
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate,
        uint maximumMessageBytes)
    {
        try
        {
            var request = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
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
        IZLinkBackendRouterSocket router,
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
                router.Reply(sourceRid, requestSeq, reply);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(reply);
            }
        });
    }
}
