using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelPublishDispatchPipeline(
    string? meshName,
    ZLinkHandlerRegistry handlerRegistry,
    ZLinkHandlerDispatcher dispatcher,
    Func<string, IReadOnlySet<string>> resolveMappedGroups,
    LogLevel unhandledLogLevel,
    ZLinkDispatchErrorReporter dispatchErrors,
    ZLinkCodecRegistryBuilder codecs,
    ILogger logger)
{
    internal ZLinkChannelPublishDispatchPipeline(
        ZLinkHandlerRegistry handlerRegistry,
        ZLinkHandlerDispatcher dispatcher,
        Func<string, IReadOnlySet<string>> resolveMappedGroups,
        LogLevel unhandledLogLevel,
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkCodecRegistryBuilder codecs,
        ILogger logger)
        : this(
            null,
            handlerRegistry,
            dispatcher,
            resolveMappedGroups,
            unhandledLogLevel,
            dispatchErrors,
            codecs,
            logger)
    {
    }

    public async Task DispatchAsync(
        string channelName,
        TopicMessage topicMessage,
        ZLinkEnvelopeHeader header,
        CancellationToken cancellationToken)
    {
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.Channel,
            "Channel",
            ZLinkDispatchMessageKind.Publish,
            "Publish",
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
            scope.Dropped(logger, dispatchErrors, unhandledLogLevel);
            return;
        }

        Dictionary<Type, object?>? decodedMessages = null;
        foreach (var endpoint in endpoints)
        {
            decodedMessages ??= new Dictionary<Type, object?>();
            if (!decodedMessages.TryGetValue(endpoint.MessageType, out var message))
            {
                if (!scope.TryDecode(
                        topicMessage.Parts,
                        endpoint.MessageType,
                        scope.ContentType!,
                        codecs,
                        logger,
                        dispatchErrors,
                        ZLinkDispatchErrorAction.Drop,
                        out message))
                    continue;

                decodedMessages.Add(endpoint.MessageType, message);
            }

            var context = new ZLinkPublishMessageContext(
                meshName,
                scope.ChannelName,
                scope.PacketName!,
                scope.ContentType,
                metadata: null,
                header.CorrelationId,
                topicMessage.Topic,
                header.Source);
            try
            {
                await dispatcher.DispatchAsync(
                        endpoint,
                        message,
                        context,
                        ZLinkHandlerDispatchKind.ClassicFanout,
                        cancellationToken)
                    .ConfigureAwait(false);

                scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Dispatched);
            }
            catch (Exception ex)
            {
                scope.HandlerException(
                    logger,
                    dispatchErrors,
                    LogLevel.Error,
                    ZLinkDispatchErrorAction.Drop,
                    ex);
            }
        }
    }
}
