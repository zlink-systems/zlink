using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorPacketDispatcher(
    Func<ZLinkSpotActorHandlerRegistry?> actorHandlers,
    Func<ZLinkSpotHandlerInvoker> handlerInvoker,
    ZLinkDispatchErrorReporter dispatchErrors,
    ILogger logger)
{
    public async ValueTask DispatchAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken)
    {
        using var currentFlow = ZLinkFlowContext.Enter(
            header.FlowId,
            header.FlowOrigin is { } streamOrigin ? (ZLinkFlowOrigin)(byte)streamOrigin : null,
            dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);
        using var dispatch = runtimeState.EnterDispatch(header);
        var scope = CreateScope(
            actor,
            header,
            ZLinkDispatchMessageKind.ActorSend,
            "ActorSend");

        scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Received);

        if (TryResolveActorPacketDescriptor(actor.GetType(), header, out var descriptor)
            && descriptor is not null)
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
                    logger,
                    dispatchErrors,
                    ZLinkDispatchErrorAction.Drop,
                    ex.DecodeException);
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

            return;
        }

        scope.Dropped(logger, dispatchErrors, LogLevel.Warning);
    }

    public async ValueTask<ZLinkActorReply?> DispatchForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken)
    {
        using var dispatch = runtimeState.EnterDispatch(header);
        var scope = CreateScope(
            actor,
            header,
            ZLinkDispatchMessageKind.ActorRequest,
            "ActorRequest");

        scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Received);

        if (TryResolveActorPacketDescriptor(actor.GetType(), header, out var descriptor)
            && descriptor is not null)
            try
            {
                var reply = await handlerInvoker()
                    .InvokeActorPacketForReplyAsync(descriptor, actor, header, body, cancellationToken)
                    .ConfigureAwait(false);

                scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Replied);

                return reply;
            }
            catch (ZLinkStreamPayloadDecodeException ex)
            {
                scope.PayloadDecodeFailed(
                    logger,
                    dispatchErrors,
                    ZLinkDispatchErrorAction.ReplyError,
                    ex.DecodeException);
                return ZLinkActorReply.FromError(ex.DecodeException);
            }
            catch (Exception ex)
            {
                scope.HandlerException(
                    logger,
                    dispatchErrors,
                    LogLevel.Error,
                    ZLinkDispatchErrorAction.ReplyError,
                    ex);
                return ZLinkActorReply.FromError(ex);
            }

        var error = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"No Spot actor request handler is registered for '{header.Name}'.");
        scope.HandlerMissing(
            logger,
            dispatchErrors,
            LogLevel.Error,
            ZLinkDispatchErrorAction.ReplyError,
            error);
        return ZLinkActorReply.FromError(error);
    }

    private static ZLinkDispatchFlowScope CreateScope(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        ZLinkDispatchMessageKind kind,
        string kindName)
    {
        return new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotActor,
            "SpotActor",
            kind,
            kindName,
            header.Name,
            correlationId: header.CorrelationId,
            actorId: actor.Context.ActorId,
            actorType: actor.GetType().FullName);
    }

    private bool TryResolveActorPacketDescriptor(
        Type actorType,
        ZlinkStreamHeader header,
        out ZLinkSpotActorPacketDescriptor? descriptor)
    {
        descriptor = null;
        return actorHandlers() is { } handlers
               && handlers.TryResolve(actorType, header, out descriptor);
    }
}
