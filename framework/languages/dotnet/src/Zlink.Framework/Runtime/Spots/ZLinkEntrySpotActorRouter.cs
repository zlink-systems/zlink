namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkEntrySpotActorRouter(ZLinkFrameworkRuntime runtime)
{
    private readonly ZLinkDispatchErrorReporter _dispatchErrors = new(
        runtime.Registration.DispatchOptions,
        runtime: runtime);

    public async ValueTask<bool> TryAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken)
    {
        foreach (var node in state.SpotNodes.Values)
        {
            var activation = node.EntrySpotActivation;
            if (activation is null
                || !activation.TryResolveActorPacket(actor.GetType(), header, out var descriptor)
                || descriptor is null)
                continue;

            // The only caller (the dispatch router's send path) already
            // holds this actor's dispatch turn; re-entering the mailbox
            // here deadlocks the actor permanently.
            try
            {
                await activation.InvokeActorPacketAsync(
                        descriptor,
                        actor,
                        header,
                        body,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (ZLinkStreamPayloadDecodeException ex)
            {
                CreateActorFlow(
                        actor,
                        header,
                        ZLinkDispatchMessageKind.ActorSend,
                        "ActorSend")
                    .PayloadDecodeFailed(
                        ZLinkStandardErrorLogger.Instance,
                        _dispatchErrors,
                        ZLinkDispatchErrorAction.Drop,
                        ex.DecodeException);
            }

            return true;
        }

        return false;
    }

    public async ValueTask<EntrySpotActorReplyDispatchResult> TrySubmitForReplyAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message body,
        bool callerOwnsDispatchTurn,
        bool relocationReplay,
        CancellationToken cancellationToken)
    {
        foreach (var node in state.SpotNodes.Values)
        {
            var activation = node.EntrySpotActivation;
            if (activation is null
                || !activation.TryResolveActorPacket(actor.GetType(), header, out var descriptor)
                || descriptor is null)
                continue;

            var flow = CreateActorFlow(
                actor,
                header,
                ZLinkDispatchMessageKind.ActorRequest,
                "ActorRequest");
            flow.Trace(_dispatchErrors, ZLinkMessageFlowOutcome.Received);

            // A caller inside the actor's dispatch turn (the dispatch
            // router) must not re-enter the mailbox — that deadlocks the
            // actor. Turnless callers (the entry pump) still serialize
            // through it.
            try
            {
                var reply = callerOwnsDispatchTurn
                    ? await activation.InvokeActorPacketForReplyAsync(
                            descriptor,
                            actor,
                            header,
                            body,
                            cancellationToken: cancellationToken)
                        .ConfigureAwait(false)
                    : await runtimeState.ExecuteDispatchAsync(
                            header,
                            ct => activation.InvokeActorPacketForReplyAsync(
                                descriptor,
                                actor,
                                header,
                            body,
                            ct),
                            countAsPendingRequest: true,
                            cancellationToken: cancellationToken,
                            allowRelocationReplay: relocationReplay)
                        .ConfigureAwait(false);
                flow.Trace(_dispatchErrors, ZLinkMessageFlowOutcome.Replied);
                return new EntrySpotActorReplyDispatchResult(true, reply);
            }
            catch (ZLinkStreamPayloadDecodeException ex)
            {
                flow.PayloadDecodeFailed(
                    ZLinkStandardErrorLogger.Instance,
                    _dispatchErrors,
                    ZLinkDispatchErrorAction.ReplyError,
                    ex.DecodeException);
                return new EntrySpotActorReplyDispatchResult(
                    true,
                    ZLinkActorReply.FromError(ex.DecodeException));
            }
            catch (Exception ex)
            {
                _dispatchErrors.Report(new ZLinkDispatchFailure(
                    ZLinkDispatchErrorSurface.SpotActor,
                    ZLinkDispatchMessageKind.ActorRequest,
                    ZLinkDispatchErrorReason.HandlerException,
                    ZLinkDispatchErrorAction.ReplyError,
                    header.Name,
                    ActorId: actor.Context.ActorId,
                    CorrelationId: header.CorrelationId,
                    Exception: ex));
                return new EntrySpotActorReplyDispatchResult(true, ZLinkActorReply.FromError(ex));
            }
        }

        return new EntrySpotActorReplyDispatchResult(false, null);
    }

    private static ZLinkDispatchFlowScope CreateActorFlow(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        ZLinkDispatchMessageKind messageKind,
        string kindName)
    {
        return new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotActor,
            "SpotActor",
            messageKind,
            kindName,
            header.Name,
            correlationId: header.CorrelationId,
            actorId: actor.Context.ActorId,
            actorType: actor.GetType().FullName);
    }

    public async ValueTask NotifyJoinedAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        RoutingId? targetNodeRid,
        CancellationToken cancellationToken)
    {
        await NotifyLifecycleAsync(
            state,
            actor,
            targetNodeRid,
            static (ZLinkEntrySpotActivation activation, Type actorType,
                    out ZLinkSpotActorLifecycleDescriptor? descriptor) =>
                activation.TryResolveActorJoined(actorType, out descriptor),
            throwOnFailure: false,
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask NotifyJoinedForRelocationAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        RoutingId targetNodeRid,
        CancellationToken cancellationToken)
    {
        await NotifyLifecycleAsync(
                state,
                actor,
                targetNodeRid,
                static (ZLinkEntrySpotActivation activation, Type actorType,
                        out ZLinkSpotActorLifecycleDescriptor? descriptor) =>
                    activation.TryResolveActorJoined(actorType, out descriptor),
                throwOnFailure: true,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkActorCreateResponse> NotifyCreatedAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        ZLinkMessage createRequest,
        RoutingId? targetNodeRid,
        CancellationToken cancellationToken)
    {
        foreach (var node in state.SpotNodes.Values)
        {
            if (targetNodeRid is not null && node.Node.RoutingId != targetNodeRid)
                continue;
            if (node.EntrySpotActivation is not { } activation
                || !activation.TryResolveActorCreated(
                    actor.GetType(),
                    out var descriptor)
                || descriptor is null)
                continue;

            return await activation.InvokeActorCreateAsync(
                    descriptor,
                    actor,
                    createRequest,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        return ZLinkActorCreateResponse.Accept();
    }

    public async ValueTask NotifyLeftAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        RoutingId? targetNodeRid,
        CancellationToken cancellationToken)
    {
        await NotifyLifecycleAsync(
            state,
            actor,
            targetNodeRid,
            static (ZLinkEntrySpotActivation activation, Type actorType,
                    out ZLinkSpotActorLifecycleDescriptor? descriptor) =>
                activation.TryResolveActorLeft(actorType, out descriptor),
            throwOnFailure: true,
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<bool> TryNotifyDisconnectedAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        RoutingId? targetNodeRid,
        CancellationToken cancellationToken)
    {
        var handled = false;
        foreach (var node in state.SpotNodes.Values)
        {
            if (targetNodeRid is not null && node.Node.RoutingId != targetNodeRid) continue;

            var activation = node.EntrySpotActivation;
            if (activation is null
                || !activation.TryResolveActorDisconnected(actor.GetType(), out var descriptor)
                || descriptor is null)
                continue;

            await activation.InvokeActorDisconnectedAsync(
                    descriptor,
                    actor,
                    cancellationToken)
                .ConfigureAwait(false);
            handled = true;
        }

        return handled;
    }


    private static async ValueTask NotifyLifecycleAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        RoutingId? targetNodeRid,
        TryResolveLifecycle resolve,
        bool throwOnFailure,
        CancellationToken cancellationToken)
    {
        await NotifyLifecycleAsync(
                state,
                actor,
                null,
                targetNodeRid,
                resolve,
                throwOnFailure,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static async ValueTask NotifyLifecycleAsync(
        ZLinkFrameworkComponentState state,
        IZLinkActor actor,
        ZLinkMessage? request,
        RoutingId? targetNodeRid,
        TryResolveLifecycle resolve,
        bool throwOnFailure,
        CancellationToken cancellationToken)
    {
        foreach (var node in state.SpotNodes.Values)
        {
            if (targetNodeRid is not null && node.Node.RoutingId != targetNodeRid) continue;
            if (node.EntrySpotActivation is not { } activation) continue;

            try
            {
                if (resolve(activation, actor.GetType(), out var descriptor)
                    && descriptor is not null)
                    await activation.InvokeActorLifecycleAsync(
                            descriptor,
                            actor,
                            request,
                            cancellationToken)
                        .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch
            {
                if (throwOnFailure) throw;
            }
        }
    }

    private delegate bool TryResolveLifecycle(
        ZLinkEntrySpotActivation activation,
        Type actorType,
        out ZLinkSpotActorLifecycleDescriptor? descriptor);
}
