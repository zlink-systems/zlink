using Systems.Zlink.Stream.Connector.Runtime;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkClientCallCodec
{
    public static ZLinkEnvelopeHeader CreateEnvelope(
        ZLinkMessageKind kind,
        string channelName,
        string messageName,
        TimeSpan? timeout = null,
        string? topic = null,
        string? source = null,
        bool includeCorrelationId = true,
        bool includeDeadline = true)
    {
        if (kind is ZLinkMessageKind.Response or ZLinkMessageKind.Error)
            throw new ArgumentOutOfRangeException(
                nameof(kind),
                kind,
                "Client call envelopes can only initiate commands, requests, or publishes.");

        var flow = ZLinkFlowContext.Current;
        var correlationRequired = kind == ZLinkMessageKind.Request;
        return new ZLinkEnvelopeHeader(
            kind,
            channelName,
            messageName,
            ZLinkEnvelopeCodec.DefaultContentType,
            includeCorrelationId || correlationRequired ? ZlinkStreamCorrelation.Next() : null,
            includeDeadline && timeout is { } value ? DateTimeOffset.UtcNow.Add(value) : null,
            topic,
            null,
            null,
            source)
        {
            FlowId = flow?.FlowId,
            FlowOrigin = flow?.Origin
        };
    }

    public static IReadOnlyList<Message> EncodeEnvelopeParts<TMessage>(
        ZLinkEnvelopeHeader header,
        TMessage message,
        ZLinkCodecRegistryBuilder? codecs)
    {
        return ZLinkEnvelopeCodec.EncodeParts(
            header,
            message,
            ZLinkClientCallTypeCache<TMessage>.Resolve(message),
            codecs);
    }

    public static TReply DecodeEnvelopeReplyAndDispose<TReply>(
        IReadOnlyList<Message> reply,
        string emptyMessage,
        string errorMessage,
        ZLinkCodecRegistryBuilder? codecs)
    {
        try
        {
            return ZLinkEnvelopeReplyDecoder.Decode<TReply>(reply, emptyMessage, errorMessage, codecs);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(reply);
        }
    }

}

internal static class ZLinkEnvelopeReplyDecoder
{
    public static TReply Decode<TReply>(
        IReadOnlyList<Message> reply,
        string emptyMessage,
        string errorMessage,
        ZLinkCodecRegistryBuilder? codecs)
    {
        if (reply.Count == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                emptyMessage);

        ZLinkEnvelopeHeader replyHeader;
        try
        {
            replyHeader = ZLinkEnvelopeCodec.DecodeHeader(reply);
        }
        catch (ZLinkFrameworkException)
        {
            throw;
        }
        catch (Exception exception)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "Reply envelope is malformed.",
                innerException: exception);
        }
        if (replyHeader.Kind == ZLinkMessageKind.Error)
            throw ZLinkEnvelopeErrorMapper.CreateException(replyHeader, errorMessage);
        if (replyHeader.Kind != ZLinkMessageKind.Response)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"Reply envelope kind '{replyHeader.Kind}' is not a response.");
        if (reply.Count < 2)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "Reply envelope body is missing.");

        try
        {
            return (TReply?)ZLinkEnvelopeCodec.DecodeBody(
                       reply,
                       typeof(TReply),
                       replyHeader.ContentType,
                       codecs)
                   ?? throw new ZLinkFrameworkException(
                       ZLinkFrameworkErrorKind.ProtocolError,
                       "Reply body is null.");
        }
        catch (ZLinkFrameworkException)
        {
            throw;
        }
        catch (Exception exception)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "Reply body could not be decoded.",
                innerException: exception);
        }
    }
}

internal static class ZLinkEnvelopeErrorMapper
{
    public static Exception CreateException(
        ZLinkEnvelopeHeader header,
        string fallbackMessage)
    {
        var message = header.ErrorMessage ?? fallbackMessage;
        if (Enum.TryParse<ZLinkFrameworkErrorKind>(header.ErrorCode, out var frameworkErrorKind)
            && Enum.IsDefined(frameworkErrorKind))
            return new ZLinkFrameworkException(frameworkErrorKind, message);

        return header.ErrorCode switch
        {
            nameof(TaskCanceledException) => new TaskCanceledException(message),
            nameof(OperationCanceledException) => new OperationCanceledException(message),
            nameof(ZLinkActorHandoffRejectedException) => new ZLinkActorHandoffRejectedException(message),
            _ => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                message)
        };
    }
}

internal static class ZLinkClientCallTypeCache<TMessage>
{
    private static readonly Type StaticType = typeof(TMessage);

    public static Type Resolve(TMessage message)
    {
        if (message is null) return StaticType;
        return StaticType.IsSealed ? StaticType : message.GetType();
    }
}
