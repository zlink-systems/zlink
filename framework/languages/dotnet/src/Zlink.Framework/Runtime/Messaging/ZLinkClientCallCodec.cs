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

    // A logical request can be reselected before admission. The body remains
    // the caller-owned typed encoding, while each attempt owns a new header
    // and native Message reference for transport handoff.
    public static Message EncodeEnvelopeBody<TMessage>(
        TMessage message,
        ZLinkCodecRegistryBuilder? codecs,
        out string contentType) =>
        ZLinkEnvelopeCodec.EncodeBody(
            message,
            ZLinkClientCallTypeCache<TMessage>.Resolve(message),
            codecs,
            out contentType);

    public static IReadOnlyList<Message> CopyEnvelopeParts(
        ZLinkEnvelopeHeader header,
        Message encodedBody,
        string contentType)
    {
        var headerPart = ZLinkEnvelopeCodec.EncodeHeader(header, contentType);
        try
        {
            return ZLinkMessageParts.Create(headerPart, encodedBody.Copy());
        }
        catch
        {
            headerPart.Dispose();
            throw;
        }
    }

    public static TReply DecodeEnvelopeReplyAndDispose<TReply>(
        IReadOnlyList<Message> reply,
        string emptyMessage,
        string errorMessage,
        ZLinkCodecRegistryBuilder? codecs,
        bool validateFlow = true) =>
        DecodeEnvelopeReplyAndDispose<TReply>(
            new ZLinkBackendRouteReceived(reply, null, null, null, null),
            emptyMessage, errorMessage, codecs, validateFlow);

    public static TReply DecodeEnvelopeReplyAndDispose<TReply>(
        ZLinkBackendRouteReceived reply,
        string emptyMessage,
        string errorMessage,
        ZLinkCodecRegistryBuilder? codecs,
        bool validateFlow = true)
    {
        try
        {
            return ZLinkEnvelopeReplyDecoder.Decode<TReply>(
                reply.Parts,
                emptyMessage,
                errorMessage,
                codecs,
                validateFlow,
                reply.ApplicationPayloadView);
        }
        finally
        {
            reply.Dispose();
        }
    }

}

internal static class ZLinkEnvelopeReplyDecoder
{
    public static TReply Decode<TReply>(
        IReadOnlyList<Message> reply,
        string emptyMessage,
        string errorMessage,
        ZLinkCodecRegistryBuilder? codecs,
        bool validateFlow = true,
        ZLinkMultipartPayloadView? applicationPayloadView = null)
    {
        var partCount = applicationPayloadView?.Count ?? reply.Count;
        if (partCount == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                emptyMessage);

        ZLinkEnvelopeHeader replyHeader;
        try
        {
            replyHeader = applicationPayloadView is { } view
                ? ZLinkEnvelopeCodec.DecodeHeader(view, validateFlow)
                : ZLinkEnvelopeCodec.DecodeHeader(reply, validateFlow);
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
        if (partCount < 2)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "Reply envelope body is missing.");

        try
        {
            var body = applicationPayloadView is { } view
                ? ZLinkEnvelopeCodec.DecodeBody(view, typeof(TReply), replyHeader.ContentType, codecs)
                : ZLinkEnvelopeCodec.DecodeBody(reply, typeof(TReply), replyHeader.ContentType, codecs);
            return (TReply?)body
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
        // Stale-route contract (C++ channel_runtime route reply mapping): only
        // framework-generated remote errors carry the zlink.origin=framework
        // marker. Unmarked remote errors came from the application handler and
        // are classified so the caller skips route-cache invalidation.
        var origin = ZLinkErrorOriginWire.RemoteReplyOrigin(header.Metadata);
        // Cross-language errorCode wire names are snake_case only; an unknown
        // name stays a protocol error.
        if (ZLinkErrorWireNames.TryParse(header.ErrorCode, out var frameworkErrorKind))
            return new ZLinkFrameworkException(frameworkErrorKind, message)
            {
                Origin = origin
            };

        return header.ErrorCode switch
        {
            nameof(TaskCanceledException) => new TaskCanceledException(message),
            nameof(OperationCanceledException) => new OperationCanceledException(message),
            nameof(ZLinkActorHandoffRejectedException) => new ZLinkActorHandoffRejectedException(message),
            _ => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                message)
            {
                Origin = origin
            }
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
