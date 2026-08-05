namespace Zlink.Framework.Runtime.Channels;

internal static class ZLinkChannelReplyWriter
{
    public static void ReplyRequest(
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs = null)
    {
        var replyParts = ZLinkEnvelopeCodec.EncodeParts(header, body, bodyType, codecs);
        var routingId = received.RoutingId
                        ?? throw new InvalidOperationException("Request reply requires a routing id.");
        ReplyParts(router, routingId, received.RequestSeq, replyParts);
    }

    public static void ReplyEnvelope(
        IZLinkBackendRouterSocket router,
        RoutingId routingId,
        ulong? requestSeq,
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs = null)
    {
        var replyParts = ZLinkEnvelopeCodec.EncodeParts(header, body, bodyType, codecs);
        ReplyParts(router, routingId, requestSeq, replyParts);
    }

    public static void ReplyRawEnvelope(
        IZLinkBackendRouterSocket router,
        RoutingId routingId,
        ulong? requestSeq,
        ZLinkEnvelopeHeader header,
        Message body)
    {
        var replyParts = ZLinkEnvelopeCodec.EncodeRawBodyParts(header, body);
        ReplyParts(router, routingId, requestSeq, replyParts);
    }

    private static void ReplyParts(
        IZLinkBackendRouterSocket router,
        RoutingId routingId,
        ulong? requestSeq,
        IReadOnlyList<Message> replyParts)
    {
        try
        {
            router.Reply(routingId, requestSeq ?? 0UL, replyParts);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    public static ZLinkEnvelopeHeader CreateReplyHeader(
        ZLinkMessageKind kind,
        string channelName,
        ZLinkEnvelopeHeader request)
    {
        return new ZLinkEnvelopeHeader(
            kind,
            channelName,
            kind == ZLinkMessageKind.Response ? string.Empty : request.MessageName,
            ZLinkEnvelopeCodec.DefaultContentType,
            request.CorrelationId,
            null,
            null,
            null,
            null)
        {
            FlowId = request.FlowId,
            FlowOrigin = request.FlowOrigin
        };
    }

    public static ZLinkEnvelopeHeader CreateErrorHeader(
        string channelName,
        ZLinkEnvelopeHeader request,
        Exception exception)
    {
        var errorCode = exception is ZLinkFrameworkException frameworkException
            ? frameworkException.Kind.ToString()
            : exception.GetType().Name;
        return new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Error,
            channelName,
            request.MessageName,
            ZLinkEnvelopeCodec.DefaultContentType,
            request.CorrelationId,
            null,
            null,
            errorCode,
            exception.Message)
        {
            FlowId = request.FlowId,
            FlowOrigin = request.FlowOrigin
        };
    }

    public static ZLinkEnvelopeHeader CreateProtocolErrorHeader(
        string channelName,
        ZLinkEnvelopeHeader request,
        string message)
    {
        var validFlow = ZLinkEnvelopeCodec.ValidFlow(request);
        return new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Error,
            channelName,
            ZLinkEnvelopeCodec.ProtocolErrorMessageName(request),
            ZLinkEnvelopeCodec.DefaultContentType,
            request.CorrelationId,
            null,
            null,
            nameof(ZLinkFrameworkErrorKind.ProtocolError),
            message)
        {
            FlowId = validFlow.FlowId,
            FlowOrigin = validFlow.FlowOrigin
        };
    }
}
