using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorDispatchRouter(
    ZLinkFrameworkRuntime runtime,
    ZLinkActorSessionRegistry actorSessions,
    Func<IZLinkActor, ZLinkActorRuntimeState, ZLinkActorContext> ensureActorContext)
{
    private readonly ZLinkDispatchErrorReporter _dispatchErrors = new(
        runtime.Registration.DispatchOptions,
        runtime.Services.GetService<ILoggerFactory>()?.CreateLogger<ZLinkActorDispatchRouter>(),
        runtime);

    private readonly ILogger _logger =
        runtime.Services.GetService<ILoggerFactory>()?.CreateLogger<ZLinkActorDispatchRouter>()
        ?? NullLogger<ZLinkActorDispatchRouter>.Instance;

    public async ValueTask SubmitByIdAsync(
        string actorId,
        ZlinkStreamHeader header,
        Message payload,
        CancellationToken cancellationToken = default)
    {
        var state = actorSessions.GetOrCreate(actorId);
        var actor = state.Actor
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorId}' is not active.");

        await Async(
                actor,
                header,
                payload,
                relocationReplay: false,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkActorReply> SubmitForReplyAsync(
        string actorId,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken = default)
    {
        var state = actorSessions.GetOrCreate(actorId);
        var actor = state.Actor
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorId}' is not active.");

        return await SubmitForReplyAsync(
                actor,
                state,
                header,
                payload,
                relocationReplay,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask Async(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken = default)
    {
        using var flow = ZLinkFlowContext.Enter(
            header.FlowId,
            header.FlowOrigin is { } streamOrigin ? (ZLinkFlowOrigin)(byte)streamOrigin : null,
            _dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);
        var actorId = actor.Context.ActorId;
        var state = actorSessions.GetOrCreate(actor.Context.ActorId);
        var shouldPrune = false;
        ensureActorContext(actor, state);

        try
        {
            shouldPrune = await state.ExecuteDispatchAsync(
                    header,
                    ct => SubmitByCurrentLocationAsync(
                        actor,
                        state,
                        header,
                        payload,
                        relocationReplay,
                        ct),
                    countAsPendingRequest: false,
                    allowRelocationReplay: relocationReplay,
                    cancellationToken: cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            if (shouldPrune) actorSessions.TryRemove(actorId, state);
        }
    }

    public async ValueTask NotifyDisconnectedByIdAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        using var flow = ZLinkFlowContext.Enter(
            null,
            null,
            _dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Lifecycle);
        var state = actorSessions.GetOrCreate(actorId);
        var actor = state.Actor
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorId}' is not active.");

        await state.ExecuteLifecycleAsync(
                ct => NotifyDisconnectedByCurrentLocationAsync(actor, state, ct),
                cancellationToken: cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkActorReply> SubmitForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken)
    {
        using var flow = ZLinkFlowContext.Enter(
            header.FlowId,
            header.FlowOrigin is { } streamOrigin ? (ZLinkFlowOrigin)(byte)streamOrigin : null,
            _dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);
        return await state.ExecuteDispatchAsync(
                header,
                ct => SubmitByCurrentLocationForReplyAsync(
                    actor,
                    state,
                    header,
                    payload,
                    relocationReplay,
                    ct),
                countAsPendingRequest: true,
                allowRelocationReplay: relocationReplay,
                cancellationToken: cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<bool> SubmitByCurrentLocationAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken)
    {
        var placement = await state.ExecuteLockedAsync(
            () => state.SelectPlacementLocked(true),
            cancellationToken).ConfigureAwait(false);

        if (placement.Activation is null)
        {
            var handled = await runtime.TrySubmitEntrySpotActorAsync(
                    actor,
                    state,
                    header,
                    payload,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!handled)
                ReportMissingHandler(
                    actor,
                    header,
                    ZLinkDispatchMessageKind.ActorSend,
                    ZLinkDispatchErrorAction.Drop);
            return placement.Prune;
        }

        var replayAdmission = relocationReplay
            ? ZLinkSpotRelocationReplayScope.Current
            : null;
        if (relocationReplay
            && replayAdmission is null
            && placement.Activation.ExecutionMode
               != ZLinkUserSpotExecutionMode.PerActor)
            throw new InvalidOperationException(
                "SPOT Actor relocation replay has no target admission.");
        await placement.Activation.SubmitActorAsync(
                actor,
                state,
                header,
                payload,
                replayAdmission,
                cancellationToken)
            .ConfigureAwait(false);
        return placement.Prune;
    }

    private async ValueTask<ZLinkActorReply> SubmitByCurrentLocationForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken)
    {
        var placement = await state.ExecuteLockedAsync(
            () => state.SelectPlacementLocked(false),
            cancellationToken).ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_dispatch_placement actor={actor.Context.ActorId} "
            + $"correlation_id={header.CorrelationId} "
            + $"activation={placement.Activation?.SpotId ?? "<entry>"} "
            + $"node={placement.Activation?.NodeRid.ToString() ?? "<entry>"}");

        if (placement.Activation is not null)
        {
            var replayAdmission = relocationReplay
                ? ZLinkSpotRelocationReplayScope.Current
                : null;
            if (relocationReplay
                && replayAdmission is null
                && placement.Activation.ExecutionMode
                   != ZLinkUserSpotExecutionMode.PerActor)
                throw new InvalidOperationException(
                    "SPOT Actor relocation replay has no target admission.");
            var reply = await placement.Activation.SubmitActorForReplyAsync(
                    actor,
                    state,
                    header,
                    payload,
                    replayAdmission,
                    cancellationToken)
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_dispatch_placement_completed actor={actor.Context.ActorId} "
                + $"correlation_id={header.CorrelationId} reply={reply is not null}");
            return reply ?? throw new InvalidOperationException(
                $"Actor request handler for '{header.Name}' returned no reply.");
        }

        var entryResult = await runtime.TrySubmitEntrySpotActorForReplyAsync(
                actor,
                state,
                header,
                payload,
                callerOwnsDispatchTurn: true,
                relocationReplay: false,
                cancellationToken)
            .ConfigureAwait(false);
        if (entryResult.Handled)
        {
            if (entryResult.Reply is { } reply)
                return reply;

            var replyPathError = new InvalidOperationException(
                $"Entry Spot actor request handler for '{header.Name}' returned no reply.");
            ReportReplyPathMissing(actor, header, replyPathError);
            return ZLinkActorReply.FromError(replyPathError);
        }

        var error = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"No Spot actor request handler is registered for '{header.Name}'.");
        ReportMissingHandler(
            actor,
            header,
            ZLinkDispatchMessageKind.ActorRequest,
            ZLinkDispatchErrorAction.ReplyError,
            error);
        return ZLinkActorReply.FromError(error);
    }

    private async ValueTask NotifyDisconnectedByCurrentLocationAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        CancellationToken cancellationToken)
    {
        var placement = await state.ExecuteLockedAsync(
            () => state.SelectPlacementLocked(false),
            cancellationToken).ConfigureAwait(false);

        if (placement.Activation is not null)
        {
            await placement.Activation.NotifyActorDisconnectedAsync(actor, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await runtime.TryNotifyEntrySpotActorDisconnectedAsync(
                actor,
                null,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private void ReportMissingHandler(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        ZLinkDispatchMessageKind kind,
        ZLinkDispatchErrorAction action,
        Exception? exception = null)
    {
        var level = action == ZLinkDispatchErrorAction.ReplyError
            ? LogLevel.Error
            : LogLevel.Warning;
        var kindText = kind == ZLinkDispatchMessageKind.ActorRequest
            ? "ActorRequest"
            : "ActorSend";
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotActor,
            "SpotActor",
            kind,
            kindText,
            header.Name,
            correlationId: header.CorrelationId,
            actorId: actor.Context.ActorId,
            actorType: actor.GetType().FullName);

        scope.HandlerMissing(
            _logger,
            _dispatchErrors,
            level,
            action,
            exception);
    }

    private void ReportReplyPathMissing(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Exception exception)
    {
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotActor,
            "SpotActor",
            ZLinkDispatchMessageKind.ActorRequest,
            "ActorRequest",
            header.Name,
            correlationId: header.CorrelationId,
            actorId: actor.Context.ActorId,
            actorType: actor.GetType().FullName);

        scope.ReplyPathMissing(_logger, _dispatchErrors, exception);
    }
}
