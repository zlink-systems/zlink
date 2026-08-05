using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelRequestDispatchPipeline(
    string? meshName,
    ZLinkHandlerRegistry handlerRegistry,
    ZLinkHandlerDispatcher dispatcher,
    Func<string, IReadOnlySet<string>> resolveMappedGroups,
    ZLinkCodecRegistryBuilder codecs,
    ZLinkDispatchErrorReporter dispatchErrors,
    ILogger logger)
{
    public async Task DispatchAsync(
        string channelName,
        IReadOnlyList<Message> parts,
        ZLinkEnvelopeHeader header,
        Func<ZLinkEnvelopeHeader, object?, Type?, ValueTask> reply,
        Func<ZLinkEnvelopeHeader, ValueTask> replyError,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata = null,
        RoutingId? sourceNodeRid = null)
    {
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.Channel,
            "Channel",
            ZLinkDispatchMessageKind.Request,
            "Request",
            header.MessageName,
            channelName,
            header.ContentType,
            header.CorrelationId);
        if (!handlerRegistry.TryGetRequest(
                channelName,
                resolveMappedGroups(channelName),
                header.MessageName,
                out var endpoint)
            || endpoint is null)
        {
            var error = new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"No request handler is registered for '{channelName}:{header.MessageName}'.");
            scope.HandlerMissing(
                logger,
                dispatchErrors,
                LogLevel.Error,
                ZLinkDispatchErrorAction.ReplyError,
                error);
            await replyError(ZLinkChannelReplyWriter.CreateErrorHeader(channelName, header, error))
                .ConfigureAwait(false);
            return;
        }

        ZLinkFrameworkException? decodeError = null;
        if (!scope.TryDecode(
                parts,
                endpoint.MessageType,
                scope.ContentType!,
                codecs,
                logger,
                dispatchErrors,
                ZLinkDispatchErrorAction.ReplyError,
                out var message,
                ex => decodeError = new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    $"PayloadDecodeFailed: failed to decode request payload for '{channelName}:{header.MessageName}'.",
                    innerException: ex)))
        {
            await replyError(ZLinkChannelReplyWriter.CreateErrorHeader(channelName, header, decodeError!))
                .ConfigureAwait(false);
            return;
        }

        IZLinkMessageContext context = sourceNodeRid is { } source
            ? new ZLinkRouteMessageContext(
                meshName,
                channelName,
                source,
                scope.PacketName!,
                scope.ContentType,
                metadata,
                header.CorrelationId)
            : new ZLinkMessageContext(
                meshName,
                channelName,
                scope.PacketName!,
                scope.ContentType,
                metadata,
                header.CorrelationId);

        try
        {
            var dispatch = await dispatcher.DispatchAsync(
                    endpoint,
                    message,
                    context,
                    ZLinkHandlerDispatchKind.ChannelRequest,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!dispatch.HandlerInvoked)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    $"A handler filter rejected '{channelName}:{header.MessageName}'.");

            await reply(
                    ZLinkChannelReplyWriter.CreateReplyHeader(ZLinkMessageKind.Response, channelName, header),
                    dispatch.Value,
                    endpoint.ReplyType)
                .ConfigureAwait(false);

            scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Replied);
        }
        catch (Exception ex)
        {
            await replyError(ZLinkChannelReplyWriter.CreateErrorHeader(channelName, header, ex))
                .ConfigureAwait(false);
            scope.HandlerException(
                logger,
                dispatchErrors,
                null,
                ZLinkDispatchErrorAction.ReplyError,
                ex);
        }
    }
}
