namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkRouteHandlerInvoker(
    ZLinkHandlerDispatcher dispatcher,
    ZLinkCodecRegistryBuilder codecs
)
{
    public async ValueTask<ZLinkHandlerDispatchResult> InvokeSendAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        IReadOnlyList<Message> parts,
        ZLinkMultipartPayloadView? payloadView,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata,
        DateTimeOffset? receivedAt,
        ZLinkDispatchFlowScope scope,
        ZLinkDispatchErrorReporter dispatchErrors
    )
    {
        var message = payloadView is { } view
            ? ZLinkEnvelopeCodec.DecodeBody(
                view,
                descriptor.MessageType,
                header.ContentType,
                codecs
            )
            : ZLinkEnvelopeCodec.DecodeBody(
                parts,
                descriptor.MessageType,
                header.ContentType,
                codecs
            );
        var ownedMessage = payloadView is not null ? message as Message : null;
        try
        {
            var context = new ZLinkRouteMessageContext(
                routerChannelId,
                null,
                sourceRid,
                header.MessageName!,
                header.ContentType,
                metadata,
                header.CorrelationId,
                receivedAt
            );
            return await dispatcher
                .DispatchRouteAsync(
                    descriptor,
                    message,
                    context,
                    ZLinkHandlerDispatchKind.NodeDirectSend,
                    cancellationToken,
                    handlerStarted: dispatchErrors.Flow.Enabled(ZLinkMessageFlowOutcome.Dispatched)
                        ? scope.CreateHandlerStartTrace(dispatchErrors)
                        : null
                )
                .ConfigureAwait(false);
        }
        finally
        {
            ownedMessage?.Dispose();
        }
    }

    public async ValueTask<ZLinkRouteHandlerReply> InvokeRequestAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata = null,
        DateTimeOffset? receivedAt = null
    )
    {
        var message = ZLinkEnvelopeCodec.DecodeBody(
            parts,
            descriptor.MessageType,
            header.ContentType,
            codecs
        );
        return await InvokeRequestAsync(
                descriptor,
                routerChannelId,
                sourceRid,
                header,
                message,
                cancellationToken,
                metadata,
                receivedAt
            )
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkRouteHandlerReply> InvokeRequestAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        ZLinkMultipartPayloadView parts,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata = null,
        DateTimeOffset? receivedAt = null
    )
    {
        var message = ZLinkEnvelopeCodec.DecodeBody(
            parts,
            descriptor.MessageType,
            header.ContentType,
            codecs
        );
        try
        {
            var reply = await InvokeRequestAsync(
                    descriptor,
                    routerChannelId,
                    sourceRid,
                    header,
                    message,
                    cancellationToken,
                    metadata,
                    receivedAt
                )
                .ConfigureAwait(false);
            return reply with { OwnedRequest = message as Message };
        }
        catch
        {
            (message as Message)?.Dispose();
            throw;
        }
    }

    private async ValueTask<ZLinkRouteHandlerReply> InvokeRequestAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        object? message,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata,
        DateTimeOffset? receivedAt
    )
    {
        var context = new ZLinkRouteMessageContext(
            routerChannelId,
            null,
            sourceRid,
            header.MessageName!,
            header.ContentType,
            metadata,
            header.CorrelationId,
            receivedAt
        );
        var dispatch = await dispatcher
            .DispatchRouteAsync(
                descriptor,
                message,
                context,
                ZLinkHandlerDispatchKind.NodeDirectRequest,
                cancellationToken
            )
            .ConfigureAwait(false);
        if (!dispatch.HandlerInvoked)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"A handler filter rejected '{header.MessageName}'."
            );

        return new ZLinkRouteHandlerReply(dispatch.Value, descriptor.ReplyType);
    }
}

internal readonly record struct ZLinkRouteHandlerReply(
    object? Message,
    Type? MessageType,
    Message? OwnedRequest = null
);
