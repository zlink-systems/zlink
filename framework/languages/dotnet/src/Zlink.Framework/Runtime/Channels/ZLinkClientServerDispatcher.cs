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
                protocolError.Message);
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
                            Reply(replyGate, router, received, replyHeader, reply, replyType);
                            return ValueTask.CompletedTask;
                        },
                        errorHeader =>
                        {
                            Reply(replyGate, router, received, errorHeader, null, null);
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
                    $"ClientServer server cannot accept '{header.Kind}' envelopes.");
                break;
        }
    }

    internal void RejectOverloaded(
        string channelName,
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate)
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
                ZLinkChannelReplyWriter.CreateErrorHeader(
                    channelName,
                    header,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Rejected,
                        $"ClientServer channel '{channelName}' application queue is full.",
                        retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff)),
                null,
                null);
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
        string message)
    {
        if (!ZLinkEnvelopeCodec.CanCorrelateReply(request))
            return;
        Reply(
            replyGate,
            router,
            received,
            ZLinkChannelReplyWriter.CreateProtocolErrorHeader(
                channelName,
                request,
                message),
            null,
            null);
    }

    private void Reply(
        ZLinkChannelReplyGate replyGate,
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType)
    {
        replyGate.TryInvoke(() =>
        {
            if (received.RoutingId is not { } sourceRid
                || received.RequestSeq is not { } requestSeq)
                return;

            var reply = ZLinkEnvelopeCodec.EncodeParts(
                header,
                body,
                bodyType,
                codecs);
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
