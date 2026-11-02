namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkRouteHandlerInvoker(
    ZLinkHandlerDispatcher dispatcher,
    ZLinkCodecRegistryBuilder codecs
)
{
    public async ValueTask InvokeSendAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata = null
    )
    {
        var message = ZLinkEnvelopeCodec.DecodeBody(
            parts,
            descriptor.MessageType,
            header.ContentType,
            codecs
        );

        var context = new ZLinkRouteMessageContext(
            routerChannelId,
            null,
            sourceRid,
            header.MessageName!,
            header.ContentType,
            metadata,
            header.CorrelationId
        );
        await dispatcher
            .DispatchRouteAsync(
                descriptor,
                message,
                context,
                ZLinkHandlerDispatchKind.NodeDirectSend,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    internal async ValueTask InvokeSendAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        ZLinkMultipartPayloadView parts,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata = null
    )
    {
        var message = ZLinkEnvelopeCodec.DecodeBody(
            parts,
            descriptor.MessageType,
            header.ContentType,
            codecs
        );
        var ownedMessage = message as Message;
        try
        {
            var context = new ZLinkRouteMessageContext(
                routerChannelId,
                null,
                sourceRid,
                header.MessageName!,
                header.ContentType,
                metadata,
                header.CorrelationId
            );
            await dispatcher
                .DispatchRouteAsync(
                    descriptor,
                    message,
                    context,
                    ZLinkHandlerDispatchKind.NodeDirectSend,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        finally
        {
            ownedMessage?.Dispose();
        }
    }

    public ValueTask<ZLinkRouteHandlerReply> InvokeRequestAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata = null
    )
    {
        var message = ZLinkEnvelopeCodec.DecodeBody(
            parts,
            descriptor.MessageType,
            header.ContentType,
            codecs
        );
        return InvokeRequestAsync(
            descriptor,
            routerChannelId,
            sourceRid,
            header,
            message,
            cancellationToken,
            metadata,
            ownedRequest: null
        );
    }

    internal ValueTask<ZLinkRouteHandlerReply> InvokeRequestAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        ZLinkMultipartPayloadView parts,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata = null
    )
    {
        var message = ZLinkEnvelopeCodec.DecodeBody(
            parts,
            descriptor.MessageType,
            header.ContentType,
            codecs
        );
        return InvokeRequestAsync(
            descriptor,
            routerChannelId,
            sourceRid,
            header,
            message,
            cancellationToken,
            metadata,
            message as Message
        );
    }

    private async ValueTask<ZLinkRouteHandlerReply> InvokeRequestAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        string routerChannelId,
        RoutingId sourceRid,
        ZLinkEnvelopeHeader header,
        object? message,
        CancellationToken cancellationToken,
        ZLinkMessageMetadata? metadata,
        Message? ownedRequest
    )
    {
        try
        {
            var context = new ZLinkRouteMessageContext(
                routerChannelId,
                null,
                sourceRid,
                header.MessageName!,
                header.ContentType,
                metadata,
                header.CorrelationId
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

            return new ZLinkRouteHandlerReply(dispatch.Value, descriptor.ReplyType, ownedRequest);
        }
        catch
        {
            ownedRequest?.Dispose();
            throw;
        }
    }
}

internal readonly record struct ZLinkRouteHandlerReply(
    object? Message,
    Type? MessageType,
    Message? OwnedRequest = null
);
