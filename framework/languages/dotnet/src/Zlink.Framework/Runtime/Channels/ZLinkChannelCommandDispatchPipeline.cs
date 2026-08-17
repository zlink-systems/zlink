namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelCommandDispatchPipeline(
    string? meshName,
    ZLinkHandlerRegistry handlerRegistry,
    ZLinkHandlerDispatcher dispatcher,
    Func<string, IReadOnlySet<string>> resolveMappedGroups,
    ZLinkDispatchErrorReporter dispatchErrors,
    ZLinkCodecRegistryBuilder codecs)
{
    public async Task DispatchAsync(
        string channelName,
        IReadOnlyList<Message> parts,
        ZLinkEnvelopeHeader header,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata = null,
        RoutingId? sourceNodeRid = null)
    {
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.Channel,
            dispatchErrors.Flow.CaptureEnabled,
            ZLinkDispatchMessageKind.Send,
            header.MessageName,
            channelName,
            header.ContentType,
            header.CorrelationId);
        if (!handlerRegistry.TryGetCommand(
                channelName,
                resolveMappedGroups(channelName),
                header.MessageName,
                out var endpoint)
            || endpoint is null)
        {
            scope.Dropped(dispatchErrors);
            return;
        }

        if (!scope.TryDecode(
                parts,
                endpoint.MessageType,
                scope.ContentType!,
                codecs,
                dispatchErrors,
                ZLinkDispatchErrorAction.Drop,
                out var message))
            return;

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
            await dispatcher.DispatchAsync(
                    endpoint,
                    message,
                    context,
                    ZLinkHandlerDispatchKind.ChannelSend,
                    cancellationToken)
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
    }
}
