namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotReplyEnvelope
{
    public static IReadOnlyList<Message> EncodeActorJoinReplyParts(
        string channelName,
        string messageName,
        object? reply,
        Type? replyType,
        ZLinkCodecRegistryBuilder? codecs = null)
    {
        var replyHeader = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            channelName,
            string.Empty,
            ZLinkEnvelopeCodec.DefaultContentType,
            null,
            null,
            null,
            null,
            null);
        return ZLinkEnvelopeCodec.EncodeParts(replyHeader, reply, replyType, codecs);
    }

    public static IReadOnlyList<Message> EncodeResponseParts(
        string channelName,
        string messageName,
        string? correlationId,
        object? reply,
        Type? replyType,
        ZLinkCodecRegistryBuilder? codecs = null)
    {
        var replyHeader = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Response,
            channelName,
            messageName,
            ZLinkEnvelopeCodec.DefaultContentType,
            correlationId,
            null,
            null,
            null,
            null);
        return ZLinkEnvelopeCodec.EncodeParts(replyHeader, reply, replyType, codecs);
    }

    public static IReadOnlyList<Message> EncodeErrorParts(
        string channelName,
        string messageName,
        string? correlationId,
        Exception exception)
    {
        var errorCode = exception is ZLinkFrameworkException frameworkException
            ? frameworkException.Kind.ToString()
            : exception.GetType().Name;
        var replyHeader = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Error,
            channelName,
            messageName,
            ZLinkEnvelopeCodec.DefaultContentType,
            correlationId,
            null,
            null,
            errorCode,
            exception.Message);
        return ZLinkEnvelopeCodec.EncodeParts(replyHeader, null, null, null);
    }

    public static IReadOnlyList<Message> EncodeProtocolErrorParts(
        string channelName,
        ZLinkEnvelopeHeader request,
        string message)
    {
        var validFlow = ZLinkEnvelopeCodec.ValidFlow(request);
        var replyHeader = new ZLinkEnvelopeHeader(
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
        return ZLinkEnvelopeCodec.EncodeParts(replyHeader, null, null, null);
    }
}
