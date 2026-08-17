namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelPublishDispatchPipeline(
    string? meshName,
    ZLinkHandlerRegistry handlerRegistry,
    ZLinkHandlerDispatcher dispatcher,
    Func<string, IReadOnlySet<string>> resolveMappedGroups,
    ZLinkDispatchErrorReporter dispatchErrors,
    ZLinkCodecRegistryBuilder codecs)
{
    internal ZLinkChannelPublishDispatchPipeline(
        ZLinkHandlerRegistry handlerRegistry,
        ZLinkHandlerDispatcher dispatcher,
        Func<string, IReadOnlySet<string>> resolveMappedGroups,
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkCodecRegistryBuilder codecs)
        : this(
            null,
            handlerRegistry,
            dispatcher,
            resolveMappedGroups,
            dispatchErrors,
            codecs)
    {
    }

    public async Task DispatchAsync(
        string channelName,
        TopicMessage topicMessage,
        ZLinkEnvelopeHeader header,
        CancellationToken cancellationToken)
    {
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.ClassicFanout,
            dispatchErrors.Flow.CaptureEnabled,
            ZLinkDispatchMessageKind.Send,
            header.MessageName,
            channelName,
            header.ContentType,
            header.CorrelationId,
            topicMessage.Topic,
            header.Source);
        var endpoints = handlerRegistry.GetPublishes(
            channelName,
            resolveMappedGroups(channelName),
            header.MessageName);
        if (endpoints.Count == 0)
        {
            if (dispatchErrors.Enabled)
                dispatchErrors.Report(new ZLinkDispatchFailure(
                    ZLinkDispatchErrorSurface.ClassicFanout,
                    ZLinkDispatchMessageKind.Send,
                    ZLinkDispatchErrorReason.HandlerMissing,
                    ZLinkDispatchErrorAction.Drop,
                    header.MessageName,
                    channelName,
                    topicMessage.Topic,
                    SourceRid: header.Source,
                    CorrelationId: header.CorrelationId));
            return;
        }

        Dictionary<Type, object?>? decodedMessages = null;
        var context = new ZLinkPublishMessageContext(
            meshName,
            scope.ChannelName,
            scope.PacketName!,
            scope.ContentType,
            metadata: null,
            header.CorrelationId,
            topicMessage.Topic,
            header.Source);
        foreach (var endpoint in endpoints)
        {
            decodedMessages ??= new Dictionary<Type, object?>();
            if (!decodedMessages.TryGetValue(endpoint.MessageType, out var message))
            {
                try
                {
                    message = ZLinkEnvelopeCodec.DecodeBody(
                        topicMessage.Parts,
                        endpoint.MessageType,
                        scope.ContentType!,
                        codecs);
                }
                catch
                {
                    // Classic fanout has no per-subscriber result. A payload that
                    // cannot be decoded for one endpoint must not create a flow or
                    // prevent another compatible endpoint from receiving it.
                    continue;
                }

                decodedMessages.Add(endpoint.MessageType, message);
            }

            try
            {
                await ZLinkApplicationJobQueueInvocation
                    .EnsureQueuedPermitAsync(cancellationToken)
                    .ConfigureAwait(false);
                await dispatcher.DispatchAsync(
                        endpoint,
                        message,
                        context,
                        ZLinkHandlerDispatchKind.ClassicFanout,
                        cancellationToken)
                    .ConfigureAwait(false);
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
}
