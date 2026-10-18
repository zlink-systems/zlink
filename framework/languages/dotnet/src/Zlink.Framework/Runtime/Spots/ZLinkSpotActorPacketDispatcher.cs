namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorPacketDispatcher(
    Func<ZLinkSpotActorHandlerRegistry?> actorHandlers,
    Func<ZLinkSpotHandlerInvoker> handlerInvoker,
    ZLinkDispatchErrorReporter dispatchErrors
)
{
    public async ValueTask DispatchAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken,
        RoutingId? sourceSessionRid
    )
    {
        using var currentFlow = ZLinkFlowContext.Enter(
            header.FlowId,
            header.FlowOrigin is { } streamOrigin
                ? ZLinkStreamHeaderCodec.ToFrameworkOrigin(streamOrigin)
                : null,
            dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound
        );
        using var dispatch = runtimeState.EnterDispatch();
        var scope = CreateScope(
            actor,
            header,
            ZLinkDispatchMessageKind.ActorSend,
            sourceSessionRid
        );

        scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Received);

        if (
            TryResolveActorPacketDescriptor(actor.GetType(), header, out var descriptor)
            && descriptor is not null
        )
        {
            try
            {
                await handlerInvoker()
                    .InvokeActorPacketAsync(descriptor, actor, header, body, cancellationToken)
                    .ConfigureAwait(false);

                scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Dispatched);
            }
            catch (ZLinkStreamPayloadDecodeException ex)
            {
                scope.PayloadDecodeFailed(
                    dispatchErrors,
                    ZLinkDispatchErrorAction.Drop,
                    ex.DecodeException
                );
            }
            catch (Exception ex)
            {
                scope.HandlerException(dispatchErrors, ZLinkDispatchErrorAction.Drop, ex);
            }

            return;
        }

        ReportMissingSendHandler(dispatchErrors, actor, header, sourceSessionRid);
    }

    public async ValueTask<ZLinkActorReply?> DispatchForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken,
        RoutingId? sourceSessionRid
    )
    {
        using var dispatch = runtimeState.EnterDispatch();
        var scope = CreateScope(
            actor,
            header,
            ZLinkDispatchMessageKind.ActorRequest,
            sourceSessionRid
        );

        scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Received);

        if (
            TryResolveActorPacketDescriptor(actor.GetType(), header, out var descriptor)
            && descriptor is not null
        )
            try
            {
                var reply = await handlerInvoker()
                    .InvokeActorPacketForReplyAsync(
                        descriptor,
                        actor,
                        header,
                        body,
                        cancellationToken
                    )
                    .ConfigureAwait(false);

                scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Replied);

                return reply;
            }
            catch (ZLinkStreamPayloadDecodeException ex)
            {
                scope.PayloadDecodeFailed(
                    dispatchErrors,
                    ZLinkDispatchErrorAction.ReplyError,
                    ex.DecodeException
                );
                return ZLinkActorReply.FromError(ex.DecodeException);
            }
            catch (Exception ex)
            {
                scope.HandlerException(dispatchErrors, ZLinkDispatchErrorAction.ReplyError, ex);
                return ZLinkActorReply.FromError(ex);
            }

        return MissingHandlerReply(dispatchErrors, actor, header, sourceSessionRid);
    }

    internal static void ReportMissingSendHandler(
        ZLinkDispatchErrorReporter errors,
        IZLinkActor actor,
        ZlinkStreamHeader header,
        RoutingId? sourceSessionRid
    )
    {
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotActor,
            errors.Flow.CaptureEnabled,
            ZLinkDispatchMessageKind.ActorSend,
            header.Name,
            correlationId: header.CorrelationId,
            actorId: actor.Context.ActorId,
            streamSessionId: errors.Enabled && sourceSessionRid is { IsEmpty: false } sessionRid
                ? sessionRid.ToHex()
                : null
        );
        scope.HandlerMissing(errors, ZLinkDispatchErrorAction.Drop);
    }

    internal static ZLinkActorReply MissingHandlerReply(
        ZLinkDispatchErrorReporter errors,
        IZLinkActor actor,
        ZlinkStreamHeader header,
        RoutingId? sourceSessionRid
    )
    {
        var error = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"No Spot actor request handler is registered for '{header.Name}'."
        );
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotActor,
            errors.Flow.CaptureEnabled,
            ZLinkDispatchMessageKind.ActorRequest,
            header.Name,
            correlationId: header.CorrelationId,
            actorId: actor.Context.ActorId,
            streamSessionId: errors.Flow.CaptureEnabled
            && sourceSessionRid is { IsEmpty: false } sessionRid
                ? sessionRid.ToHex()
                : null
        );
        scope.HandlerMissing(errors, ZLinkDispatchErrorAction.ReplyError, error);
        return ZLinkActorReply.FromError(error);
    }

    private ZLinkDispatchFlowScope CreateScope(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        ZLinkDispatchMessageKind kind,
        RoutingId? sourceSessionRid
    )
    {
        return new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotActor,
            dispatchErrors.Flow.CaptureEnabled,
            kind,
            header.Name,
            correlationId: header.CorrelationId,
            actorId: actor.Context.ActorId,
            streamSessionId: dispatchErrors.Flow.CaptureEnabled
            && sourceSessionRid is { IsEmpty: false } sessionRid
                ? sessionRid.ToHex()
                : null
        );
    }

    private bool TryResolveActorPacketDescriptor(
        Type actorType,
        ZlinkStreamHeader header,
        out ZLinkSpotActorPacketDescriptor? descriptor
    )
    {
        descriptor = null;
        return actorHandlers() is { } handlers
            && handlers.TryResolve(actorType, header, out descriptor);
    }
}
