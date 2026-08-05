namespace Zlink.Framework.Runtime.Streams;

using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkSessionActorCoordinator(
    ZLinkFrameworkRuntime runtime,
    IZLinkStream stream,
    bool actorDispatchEnabled)
{
    private readonly ZLinkSessionActorBindingRegistry _bindings =
        new(runtime);
    private readonly object _actorOperationGatesLock = new();
    private readonly Dictionary<string, ActorOperationGate> _actorOperationGates =
        new(StringComparer.Ordinal);

    public IReadOnlyCollection<IZLinkSessionActor> BoundActors => _bindings.BoundActors;

    public ValueTask<IZLinkSessionActor> BindActorAsync(
        ZLinkSessionContext context,
        ActorRef actor,
        CancellationToken cancellationToken)
    {
        return BindActorSerializedAsync(
            context,
            actor,
            allowExisting: false,
            cancellationToken: cancellationToken);
    }

    public ValueTask<IZLinkSessionActor> BindOrGetActorAsync(
        ZLinkSessionContext context,
        ActorRef actor,
        CancellationToken cancellationToken)
    {
        return BindActorSerializedAsync(
            context,
            actor,
            allowExisting: true,
            cancellationToken: cancellationToken);
    }

    private async ValueTask<IZLinkSessionActor> BindActorSerializedAsync(
        ZLinkSessionContext context,
        ActorRef actor,
        bool allowExisting,
        CancellationToken cancellationToken)
    {
        if (!actorDispatchEnabled)
            throw new ZLinkConfigurationException(
                "STREAM Actor dispatch requires EnableActorDispatch().");
        ActorOperationGate operation;
        lock (_actorOperationGatesLock)
        {
            if (!_actorOperationGates.TryGetValue(actor.ActorId, out operation!))
            {
                operation = new ActorOperationGate();
                _actorOperationGates.Add(actor.ActorId, operation);
            }
            operation.Users++;
        }
        var acquired = false;
        try
        {
            await operation.Gate.WaitAsync(cancellationToken).ConfigureAwait(false);
            acquired = true;
            return await BindActorWithinGateAsync(
                    context,
                    actor,
                    allowExisting,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            if (acquired)
                operation.Gate.Release();
            lock (_actorOperationGatesLock)
            {
                operation.Users--;
                if (operation.Users == 0
                    && _actorOperationGates.Remove(actor.ActorId, out var removed)
                    && ReferenceEquals(removed, operation))
                    operation.Gate.Dispose();
            }
        }
    }

    private sealed class ActorOperationGate
    {
        internal SemaphoreSlim Gate { get; } = new(1, 1);
        internal int Users { get; set; }
    }

    private async ValueTask<IZLinkSessionActor> BindActorWithinGateAsync(
        ZLinkSessionContext context,
        ActorRef actor,
        bool allowExisting,
        CancellationToken cancellationToken)
    {
        if (_bindings.FindActor(actor.ActorId) is { } existing)
        {
            if (allowExisting && ActorRefsEqual(existing.Ref, actor))
                return existing;

            if (existing is not ZLinkSessionActor existingActor)
                throw new InvalidOperationException("Actor ref was not created by this framework runtime.");
            if (!runtime.TryGetSessionActorBinding(
                    existing.ActorId,
                    existingActor.BindingToken,
                    out var previousEntry))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{existing.ActorId}' replacement lost its previous binding identity.");
            var previousIdentity = new ZLinkActorBoundSession(
                runtime.IsStarted
                    ? runtime.GetMeshNodeRuntime(previousEntry.MeshName)
                        .Node.RoutingId
                    : null,
                existingActor.SessionRid,
                previousEntry.BindingToken,
                previousEntry.BindingGeneration,
                previousEntry.ObjectGeneration,
                previousEntry.AuthorityOwnerGeneration,
                previousEntry.MeshName,
                previousEntry.TargetNodeGeneration,
                previousEntry.OwnerLeaseGeneration,
                previousEntry.SessionOwnerNodeGeneration,
                previousEntry.AcceptedHighWater);
            IZLinkSessionActor replacement;
            try
            {
                // BindAsync replaces the previous table entry only after the
                // new exact owner has acknowledged. Until then the previous
                // binding remains the terminal route for this session.
                replacement = await BindActorCoreAsync(
                        context,
                        actor,
                        new ZLinkRemoteSessionPreviousBinding(
                            existing.Ref.NodeRid.ToBytes().ToArray(),
                            previousIdentity.SessionNodeRid?.ToBytes().ToArray()
                            ?? Array.Empty<byte>(),
                            previousIdentity.SessionRid.ToBytes().ToArray(),
                            previousIdentity.BindingToken,
                            previousIdentity.BindingGeneration,
                            previousIdentity.ObjectGeneration,
                            previousIdentity.MeshName,
                            previousIdentity.TargetNodeGeneration,
                            previousIdentity.AuthorityOwnerGeneration,
                            previousIdentity.OwnerLeaseGeneration,
                            previousIdentity.SessionOwnerNodeGeneration,
                            previousIdentity.AcceptedHighWater),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception replacementFailure)
            {
                // A started runtime never created a native session binding for
                // the previous Actor. BindAsync has not replaced the table
                // entry yet, so the existing framework route is already the
                // complete rollback state.
                if (runtime.IsStarted || !IsLocalActorRef(existing.Ref)) throw;
                try
                {
                    await BindNativeActorAsync(
                            existing.Ref.ToBackend(),
                            CancellationToken.None)
                        .ConfigureAwait(false);
                }
                catch (Exception rollbackFailure)
                {
                    throw new AggregateException(
                        replacementFailure,
                        rollbackFailure);
                }

                throw;
            }
            return replacement;
        }

        return await BindActorCoreAsync(context, actor, null, cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<IZLinkSessionActor> BindActorCoreAsync(
        ZLinkSessionContext context,
        ActorRef actor,
        ZLinkRemoteSessionPreviousBinding? previousBinding,
        CancellationToken cancellationToken)
    {
        ZLinkSessionBindingIdentity? confirmedIdentity = null;
        EnsureConcreteActorRef(actor);
        var actorRef = actor.ToBackend();
        // A started runtime always confirms through the ActorRef MeshName so
        // local and remote owners use one global authority route. Direct
        // pre-start test contexts retain the native binding boundary.
        var nativeBound = !runtime.IsStarted;
        if (nativeBound)
            await BindNativeActorAsync(actorRef, cancellationToken).ConfigureAwait(false);
        try
        {
            var identity = await ConfirmBindingAsync(
                    context,
                    actor,
                    previousBinding,
                    cancellationToken)
                .ConfigureAwait(false);
            confirmedIdentity = identity;
            return await _bindings.BindAsync(
                context,
                actor,
                identity.BindingToken,
                identity.BindingGeneration,
                identity.AuthorityOwnerGeneration,
                identity.MeshName,
                identity.TargetNodeGeneration,
                identity.OwnerLeaseGeneration,
                identity.SessionOwnerNodeGeneration,
                // Remote acknowledgement publishes the Actor owner's
                // terminal binding. The source table must finish the
                // matching local commit even if caller cancellation races
                // that acknowledgement.
                CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception bindingFailure)
        {
            try
            {
                if (!runtime.IsStarted
                    && confirmedIdentity is { } localIdentity)
                    runtime.UnbindActorSession(
                        actor.ActorId,
                        localIdentity.BindingToken);
                if (nativeBound)
                    await UnbindNativeActorAsync(actor.ActorId, CancellationToken.None)
                        .ConfigureAwait(false);
            }
            catch (Exception rollbackFailure)
            {
                throw new AggregateException(bindingFailure, rollbackFailure);
            }

            throw;
        }
    }

    private bool IsLocalActorRef(ActorRef actor)
    {
        if (!runtime.IsStarted) return true;
        return actor.NodeRid == runtime.GetMeshNodeRuntime(actor.MeshName).Node.RoutingId;
    }

    private async ValueTask<ZLinkSessionBindingIdentity> ConfirmBindingAsync(
        ZLinkSessionContext context,
        ActorRef actor,
        ZLinkRemoteSessionPreviousBinding? previousBinding,
        CancellationToken cancellationToken)
    {
        // Contexts constructed before runtime startup are used only by the
        // transport-level unit boundary; no remote route exists to confirm.
        var bindingGeneration = runtime.NextSessionBindingGeneration();
        if (!runtime.IsStarted)
        {
            if (context.RoutingId is not { } localSessionRid)
                throw new InvalidOperationException(
                    "Actor session binding requires a stream routing id.");
            var localBindingToken = Guid.NewGuid().ToString("N");
            const ulong localGeneration = 1;
            runtime.BindActorSession(
                actor.ActorId,
                null,
                localSessionRid,
                localBindingToken,
                bindingGeneration,
                actor.ObjectGeneration,
                localGeneration,
                actor.MeshName,
                localGeneration,
                localGeneration,
                localGeneration);
            return new ZLinkSessionBindingIdentity(
                localBindingToken,
                bindingGeneration,
                actor.MeshName,
                TargetNodeGeneration: localGeneration,
                OwnerLeaseGeneration: localGeneration,
                AuthorityOwnerGeneration: localGeneration,
                SessionOwnerNodeGeneration: localGeneration);
        }
        if (context.RoutingId is not { } sessionRid)
            throw new InvalidOperationException("Actor session binding requires a stream routing id.");
        var sessionNode = runtime.GetMeshNodeRuntime(actor.MeshName).Node;
        var sessionNodeRid = sessionNode.RoutingId;
        var sessionOwnerNodeGeneration = sessionNode.MeshStatus().LifecycleGeneration;
        if (sessionOwnerNodeGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Session owner lifecycle generation is unavailable.");
        var bindingToken = Guid.NewGuid().ToString("N");
        var request = new ZLinkRemoteSessionBindRequest(
            actor.ActorId,
            actor.NodeRid.ToBytes().ToArray(),
            sessionNodeRid.ToBytes().ToArray(),
            sessionRid.ToBytes().ToArray(),
            bindingToken,
            bindingGeneration,
            actor.ObjectGeneration,
            actor.MeshName,
            sessionOwnerNodeGeneration,
            AcceptedHighWater: 0,
            PreviousBinding: previousBinding);
        // The bind confirm can race auto-discovery admission of the actor's
        // node at startup; retriable route failures retry within the request
        // timeout instead of failing the session's first authenticate.
        var deadline = DateTime.UtcNow + runtime.Registration.DefaultRequestTimeout;
        ZLinkRemoteSessionBindResponse response;
        while (true)
        {
            try
            {
                response = actor.NodeRid == sessionNodeRid
                    ? await runtime.BindRemoteBoundSessionRouteAsync(
                            request,
                            sessionNodeRid,
                            cancellationToken)
                        .ConfigureAwait(false)
                    : await runtime.Services.GetRequiredService<IZLinkRouteClient>()
                        .RequestToNode(actor.MeshName, actor.NodeRid, request)
                        .Timeout(runtime.Registration.DefaultRequestTimeout)
                        .Async<ZLinkRemoteSessionBindResponse>(cancellationToken)
                        .ConfigureAwait(false);
                break;
            }
            catch (ZLinkFrameworkException failure)
                when ((failure.RetryAdvice != ZLinkRetryAdvice.DoNotRetry
                       || failure.Kind == ZLinkFrameworkErrorKind.NotFound)
                      && DateTime.UtcNow < deadline)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken)
                    .ConfigureAwait(false);
            }
        }

        if (!response.Acknowledged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actor.ActorId}' did not acknowledge its remote session binding.");
        var responseTargetNodeRid = RoutingId.From(response.TargetNodeRid);
        if (response.ObjectGeneration != actor.ObjectGeneration
            || responseTargetNodeRid != actor.NodeRid
            || !string.Equals(response.MeshName, actor.MeshName, StringComparison.Ordinal)
            || response.TargetNodeGeneration == 0
            || response.AuthorityOwnerGeneration == 0
            || response.OwnerLeaseGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actor.ActorId}' returned a different route during session bind.");
        return new ZLinkSessionBindingIdentity(
            bindingToken,
            bindingGeneration,
            response.MeshName,
            response.TargetNodeGeneration,
            response.OwnerLeaseGeneration,
            response.AuthorityOwnerGeneration,
            sessionOwnerNodeGeneration);
    }

    public IZLinkSessionActor? FindActor(string actorId)
    {
        return _bindings.FindActor(actorId);
    }

    public async ValueTask RelayToActorAsync(
        IZLinkSessionActor actor,
        ZlinkStreamHeader header,
        Message payload,
        Func<ZlinkStreamHeader, ZLinkActorReply, CancellationToken, ValueTask> replyRawAsync,
        CancellationToken cancellationToken)
    {
        if (actor is not ZLinkSessionActor actorRef)
            throw new InvalidOperationException("Actor ref was not created by this framework runtime.");

        var contextLive = runtime.TryGetSessionActorContext(
            actorRef.ActorId,
            actorRef.BindingToken,
            out var currentContext)
            && ReferenceEquals(currentContext, actorRef.Context);
        //  This guard checks the binding token only. An Actor destroyed and
        //  recreated keeps the same token, so a stale incarnation passes here.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"session_relay_entry actor={actorRef.ActorId} context_live={contextLive} "
            + $"has_route={actorRef.TryGetRoute(out var relayRoute)} "
            + $"route_authority_gen={relayRoute.AuthorityOwnerGeneration} "
            + $"route_node_gen={relayRoute.TargetNodeGeneration}");
        if (!contextLive)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorRef.ActorId}' session binding is stale.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        var isBindingControlFrame = string.Equals(
            header.Name,
            ZLinkRemoteSessionBindingProtocol.PacketName,
            StringComparison.Ordinal);
        var acceptedFrame = false;
        ulong acceptedHighWater = 0;
        if (!isBindingControlFrame)
        {
            // A relocation seal is an infrastructure boundary, not an
            // application rejection. Keep this frame in the current stream
            // operation until the session owner publishes the new route.
            while (!runtime.TryAcceptSessionActorFrame(
                       actorRef.ActorId,
                       actorRef.BindingToken,
                       out acceptedHighWater))
            {
                if (!await runtime.WaitForSessionActorRouteAvailableAsync(
                            actorRef.ActorId,
                            actorRef.BindingToken,
                            cancellationToken)
                        .ConfigureAwait(false))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Actor '{actorRef.ActorId}' session binding changed before frame admission.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
            }
            acceptedFrame = true;
        }

        try
        {
            if (stream is ZLinkManagedStream)
            {
                var route = actorRef.Route;
                if (!IsLocalActorRef(route))
                {
                    var requestId = header.Kind == ZlinkStreamMessageKind.Request
                        ? header.RequestSeq
                        : null;
                    string? replyCapability = null;
                    if (requestId is { } pendingRequestId)
                    {
                        replyCapability = runtime.TrackRemoteSessionActorRequest(
                            actorRef.ActorId,
                            pendingRequestId.Value,
                            actorRef.BindingToken);
                        acceptedFrame = false;
                    }
                    try
                    {
                        await ForwardToRemoteActorAsync(
                                actorRef,
                                header,
                                payload,
                                replyCapability,
                                acceptedHighWater,
                                cancellationToken)
                            .ConfigureAwait(false);
                    }
                    catch
                    {
                        if (requestId is { } failedRequestId)
                            runtime.CompleteRemoteSessionActorRequest(
                                actorRef.ActorId,
                                actorRef.Ref.ObjectGeneration,
                                actorRef.BindingToken,
                                failedRequestId.Value);
                        throw;
                    }
                    return;
                }
            }

            if (!isBindingControlFrame)
                runtime.GetOrCreateActorState(actorRef.ActorId)
                    .RecordBoundSessionAccepted(actorRef.BindingToken);
            await DispatchLocalAsync(actorRef, header, payload, replyRawAsync, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            if (acceptedFrame)
                runtime.CompleteAcceptedSessionActorFrame(
                    actorRef.ActorId,
                    actorRef.BindingToken);
        }
    }

    // Session frame for an actor hosted on another node: relayed to the
    // actor's owner node on the actor-frame plane (spec 31 §6). The relay
    // carries this session's identity, so the target binds the remote route
    // and replies/pushes travel back on the push relay.
    private async ValueTask ForwardToRemoteActorAsync(
        ZLinkSessionActor actorRef,
        ZlinkStreamHeader header,
        Message payload,
        string? replyCapability,
        ulong acceptedHighWater,
        CancellationToken cancellationToken)
    {
        if (actorRef.Context.RoutingId is not { } sessionRid)
            throw new InvalidOperationException("Actor session relay requires a stream routing id.");
        var route = actorRef.Route;
        var sessionNode = runtime.GetMeshNodeRuntime(route.MeshName);
        var sessionNodeRid = sessionNode.Node.RoutingId;
        var headerBytes = ZLinkStreamProtocolDefaults.EncodeHeader(header).ToArray();
        var bodyBytes = payload.ToArray();
        var target = route.Ref.ToBackend();
        var operationId = sessionNode.Node.AllocateOperationId();
        if (!runtime.TryGetSessionActorBinding(
                actorRef.ActorId,
                actorRef.BindingToken,
                out var sessionBinding))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorRef.ActorId}' session binding changed before relay.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        var replyRoute = new ZLinkBackendActorRouteContext(
            OperationId: operationId,
            MessageFollowHopCount: 0,
            TargetNodeGeneration: route.TargetNodeGeneration,
            AuthorityOwnerGeneration: route.AuthorityOwnerGeneration,
            OwnerLeaseGeneration: route.OwnerLeaseGeneration,
            ReplyRequestId: header.Kind == ZlinkStreamMessageKind.Request
                            && header.RequestSeq is { } requestSeq
                ? requestSeq.Value
                : 0,
            ReplyFlags: header.Kind == ZlinkStreamMessageKind.Request
                ? ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind
                : 0,
            ReplyCapability: replyCapability,
            IsBoundSessionRoute: true);
        var source = sessionNode.LocalRequestSource;
        var handoffMetadata = ZLinkActorBoundSessionHandoffMetadata.Encode(
            new ZLinkActorBoundSessionHandoffFence(
                actorRef.ActorId,
                actorRef.Ref.ObjectGeneration,
                sessionRid,
                actorRef.BindingToken,
                sessionBinding.BindingGeneration,
                acceptedHighWater));
        await ZLinkRetryingSubmitter.Async(
                () =>
                {
                    using var headerPart = Message.From(headerBytes);
                    if (!runtime.ForwardActorBoundSessionPart(
                            route.MeshName,
                            target,
                            route.TargetNodeGeneration,
                            route.AuthorityOwnerGeneration,
                            route.OwnerLeaseGeneration,
                            sessionNodeRid,
                            sessionRid,
                            headerPart,
                            true,
                            SendFlags.DontWait,
                            replyRoute,
                            source.NodeGeneration,
                            source,
                            handoffMetadata))
                        return false;
                    using var bodyPart = Message.From(bodyBytes);
                    return runtime.ForwardActorBoundSessionPart(
                        route.MeshName,
                        target,
                        route.TargetNodeGeneration,
                        route.AuthorityOwnerGeneration,
                        route.OwnerLeaseGeneration,
                        sessionNodeRid,
                        sessionRid,
                        bodyPart,
                        false,
                        SendFlags.DontWait,
                        replyRoute,
                        source.NodeGeneration,
                        source,
                        handoffMetadata);
                },
                runtime.Registration.DefaultRequestTimeout,
                "Remote actor session relay failed because the relay route was not ready before timeout.",
                cancellationToken)
            .ConfigureAwait(false);
    }

    private bool IsLocalActorRef(ZLinkSessionBindingRoute route)
    {
        if (!runtime.IsStarted) return true;
        var local = runtime.GetMeshNodeRuntime(route.MeshName).Node;
        return route.Ref.NodeRid == local.RoutingId
               && route.TargetNodeGeneration
               == local.MeshStatus().LifecycleGeneration;
    }

    public ValueTask NotifyActorDisconnectedAsync(
        IZLinkSessionActor actor,
        CancellationToken cancellationToken)
    {
        if (actor is not ZLinkSessionActor actorRef)
            throw new InvalidOperationException("Actor ref was not created by this framework runtime.");

        var found = runtime.TryGetSessionActorBinding(
            actorRef.ActorId,
            actorRef.BindingToken,
            out var binding);
        if (!found
            || !ReferenceEquals(binding.ActorRef, actorRef)
            || binding.Context.RoutingId is null)
        {
            //  Returning quietly here is indistinguishable from a disconnect
            //  that was delivered, so name the condition that skipped it.
            return ValueTask.CompletedTask;
        }
        return runtime.NotifyActorDisconnectedAsync(
            binding,
            cancellationToken);
    }

    public ValueTask CleanupAsync(
        ZLinkSessionContext context,
        CancellationToken cancellationToken)
    {
        return _bindings.CleanupAsync(context, cancellationToken);
    }

    private async ValueTask DispatchLocalAsync(
        ZLinkSessionActor actorRef,
        ZlinkStreamHeader header,
        Message payload,
        Func<ZlinkStreamHeader, ZLinkActorReply, CancellationToken, ValueTask> replyRawAsync,
        CancellationToken cancellationToken)
    {
        using var actorPayload = payload.Copy();
        // Only genuine requests take the reply path (a relayed Send can
        // carry a request seq from stream-level bookkeeping).
        if (header.Kind == ZlinkStreamMessageKind.Request && header.RequestSeq is not null)
        {
            await using var boundSessionScope = ZLinkBoundSessionDispatchScope.Enter(actorRef.ActorId);
            try
            {
                var reply = await runtime.SubmitActorForReplyAsync(
                        actorRef.ActorId,
                        header,
                        actorPayload,
                        cancellationToken)
                    .ConfigureAwait(false);
                await replyRawAsync(header, reply, cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                await boundSessionScope.DrainAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
            return;
        }

        await runtime.SubmitActorByIdAsync(
                actorRef.ActorId,
                header,
                actorPayload,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static bool ActorRefsEqual(ActorRef left, ActorRef right)
    {
        return left.ActorId == right.ActorId
               && left.NodeRid == right.NodeRid
               && left.ObjectGeneration == right.ObjectGeneration
               && string.Equals(left.MeshName, right.MeshName, StringComparison.Ordinal);
    }

    private static void EnsureConcreteActorRef(ActorRef actor)
    {
        if (actor.NodeRid.IsEmpty || actor.ObjectGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                "Actor ref requires a target SpotNode routing id and concrete actor generation.",
                ZLinkRetryAdvice.DoNotRetry);
    }

    private async ValueTask BindNativeActorAsync(
        ZLinkBackendActorRef actorRef,
        CancellationToken cancellationToken)
    {
        await ZLinkNativeActorStreamBinding.BindAsync(
                stream,
                actorRef,
                runtime.Registration.DefaultRequestTimeout,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask UnbindNativeActorAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        await ZLinkNativeActorStreamBinding.UnbindAsync(
                stream,
                actorId,
                runtime.Registration.DefaultRequestTimeout,
                cancellationToken)
            .ConfigureAwait(false);
    }
}

internal readonly record struct ZLinkSessionBindingIdentity(
    string BindingToken,
    ulong BindingGeneration,
    string MeshName,
    ulong TargetNodeGeneration,
    ulong OwnerLeaseGeneration,
    ulong AuthorityOwnerGeneration,
    ulong SessionOwnerNodeGeneration);
