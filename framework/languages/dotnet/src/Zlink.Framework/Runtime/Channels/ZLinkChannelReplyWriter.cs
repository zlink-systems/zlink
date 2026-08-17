namespace Zlink.Framework.Runtime.Channels;

internal static class ZLinkChannelReplyWriter
{
    public static void ReplyRequest(
        IRouterSocket router,
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
        IRouterSocket router,
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
        IRouterSocket router,
        RoutingId routingId,
        ulong? requestSeq,
        ZLinkEnvelopeHeader header,
        Message body)
    {
        var replyParts = ZLinkEnvelopeCodec.EncodeRawBodyParts(header, body);
        ReplyParts(router, routingId, requestSeq, replyParts);
    }

    private static void ReplyParts(
        IRouterSocket router,
        RoutingId routingId,
        ulong? requestSeq,
        IReadOnlyList<Message> replyParts)
    {
        try
        {
            router.Reply(routingId, requestSeq ?? 0UL)
                .Messages(replyParts)
                .Submit();
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    // Spec 27 §4/§7: replies preserve the request's correlation id here; the
    // two flow fields are filled from the ambient flow context at encode time
    // (ZLinkEnvelopeCodec.EncodeHeader), which exists only while tracing is
    // on, so an Off host adds no flow fields to reply envelopes.
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
            null);
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
            exception.Message);
    }

    public static ZLinkEnvelopeHeader CreateProtocolErrorHeader(
        string channelName,
        ZLinkEnvelopeHeader request,
        string message)
    {
        return new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Error,
            channelName,
            ZLinkEnvelopeCodec.ProtocolErrorMessageName(request),
            ZLinkEnvelopeCodec.DefaultContentType,
            request.CorrelationId,
            null,
            null,
            nameof(ZLinkFrameworkErrorKind.ProtocolError),
            message);
    }
}
