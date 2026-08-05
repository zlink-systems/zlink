using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Backend.DotNet.Mappings;
using Zlink.Framework.Runtime.Spots;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// RouteMesh 10.0.0 MeshNode-backed implementation of the framework SpotNode seam.
// The 9.x SpotNode fluent+callback surface is bridged onto IMeshNode: requests
// return an out MeshOperationId whose reply is resolved by the node dispatch pump
// through the completion table, and pull dispatch replaces the per-spot receiver
// loops.
internal sealed class ZLinkBackendSpotNodeWrapper :
    IZLinkBackendSpotNode,
    IZLinkBackendRelocationReplyRelay,
    IZLinkBackendCanonicalRelocationReservation,
    IZLinkBackendAuthorityObserver,
    IZLinkBackendRequestSourceFenceObserver,
    IZLinkBackendLocalActorAuthorityReader,
    IZLinkBackendActorMessageFollowIngress,
    IZLinkBackendMessageFollowNotifications
{
    private readonly IMeshNode _node;
    private readonly ZLinkMeshCompletionTable _completions = new();
    private readonly ZLinkMeshDispatchPump _pump;
    private readonly ActorMessageFollowIngressAdapter _messageFollowIngress;
    private readonly ConcurrentDictionary<string, ulong> _peerIntents =
        new(StringComparer.Ordinal);
    private readonly ZLinkSpotSubscriptionTracker _subscriptions = new();
    private readonly object _forwardGate = new();
    private readonly Dictionary<ZLinkBackendActorRef, List<Message>> _forwardBuffers = new();
    private readonly object _lifecycleGate = new();
    private readonly object _entrySpotGate = new();
    private IZLinkBackendSpot? _entrySpot;

    // First registered mesh channel; spot wrappers publish/subscribe on it
    // (Spot logical multicast uses the router plane).
    private bool _bound;
    private bool _started;
    private bool _disposed;

    public ZLinkBackendSpotNodeWrapper(IMeshNode node)
    {
        _node = node;
        _completions = new ZLinkMeshCompletionTable(
            meshName: (node as ZLinkManagedMeshNode)?.MeshName);
        _pump = new ZLinkMeshDispatchPump(node, _completions);
        _node.SetCompletionOverflowHandler(
            (record, parts) => _completions.Complete(record, parts));
        _messageFollowIngress = new ActorMessageFollowIngressAdapter(_pump);
        _node.SetActorMessageFollowIngressTarget(_messageFollowIngress);
    }

    internal IMeshNode NativeNode => _node;

    internal ZLinkMeshDispatchPump Pump => _pump;

    internal ZLinkMeshCompletionTable Completions => _completions;

    public RoutingId RoutingId => _node.RoutingId;

    public void SetLocalOwnerLeaseGeneration(ulong ownerLeaseGeneration) =>
        RequireManagedNode().SetLocalOwnerLeaseGeneration(ownerLeaseGeneration);

    public void SetLocalRequestSourceFence(
        ZLinkServiceWireCodec.RequestSourceFence source) =>
        RequireManagedNode().SetLocalRequestSourceFence(source);

    public void ObserveRequestSourceFence(
        ZLinkServiceWireCodec.RequestSourceFence source) =>
        _pump.ObserveRequestSourceFence(source);

    public void SetActorMessageFollowIngressHandler(
        Func<IReadOnlyList<ZLinkBackendActorPart>, bool> handler) =>
        _messageFollowIngress.SetHandler(handler);

    public void SetActorMessageFollowIngressAdmission(
        Func<ActorMessageFollowIngress, bool> admission) =>
        _messageFollowIngress.SetAdmission(admission);

    public void SetMessageFollowNotificationHandler(
        Action<RoutingId, ZLinkServiceWireCodec.MessageFollowRecord> handler) =>
        RequireManagedNode().SetMessageFollowNotificationHandler(handler);

    public bool TrySendMessageFollowNotification(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.MessageFollowRecord record) =>
        RequireManagedNode().TrySendMessageFollowNotification(
            targetNodeRid,
            record);

    public void ObserveActorAuthority(
        ZLinkBackendActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        RequireManagedNode().ObserveActorAuthority(
            ToNativeActor(actor),
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
    }

    public void ObserveSpotAuthority(
        RoutingId nodeRid,
        string spotId,
        ulong objectGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        RequireManagedNode().ObserveSpotAuthority(
            nodeRid,
            spotId,
            objectGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
    }

    public bool TryGetLocalActorAuthority(
        ZLinkBackendActorRef actor,
        out ulong authorityOwnerGeneration,
        out ulong ownerLeaseGeneration) =>
        RequireManagedNode().TryGetActorAuthority(
            ToNativeActor(actor),
            out authorityOwnerGeneration,
            out ownerLeaseGeneration);

    private ZLinkManagedMeshNode RequireManagedNode() =>
        _node as ZLinkManagedMeshNode
        ?? throw new InvalidOperationException(
            "Authority fencing requires the Framework managed MeshNode.");

    private ActorRef ToNativeActor(ZLinkBackendActorRef actor) =>
        actor.ToNative(RequireManagedNode().MeshName);

    public void SetRoutingId(RoutingId routingId)
    {
        _node.SetRoutingId(routingId);
    }

    public void SetObjectRole(ZLinkMeshNodeObjectRole objectRole)
    {
        _node.SetObjectRole(objectRole);
    }

    // Pub/sub routing ids and role config have no MeshNode equivalent (publishing
    // is via IMeshNode.CreatePublisher / channels). Preserved as no-ops so the
    // configuration plane keeps compiling; see S8 follow-up.
    public void SetPublisherRoutingId(RoutingId routingId)
    {
    }

    public void SetSubscriberRoutingId(RoutingId routingId)
    {
    }

    public void SetRouterBind(string endpoint)
    {
        BindOnce(endpoint);
    }

    public void SetRouterAdvertisedEndpoint(string endpoint)
    {
        _node.SetAdvertisedEndpoint(endpoint);
    }

    public void SetPubBind(string endpoint)
    {
        BindOnce(endpoint);
    }

    private void BindOnce(string endpoint)
    {
        lock (_lifecycleGate)
        {
            if (_bound) return;
            _bound = true;
            _node.SetBind(endpoint);
        }
    }

    // Startup channel sequencing (spec 21-mesh-node §3): AddChannel/SetChannelWeight
    // are applied before Start. SetChannelWeight also backs the live weight path
    // (spec 21 §4); a positive weight is a runtime descriptor-revision bump.
    public void AddChannel(string channelName)
    {
        _node.AddChannel(channelName);
    }

    public void SetChannelWeight(string channelName, uint weight)
    {
        _node.SetChannelWeight(channelName, weight);
    }

    public void PublishDraining()
    {
        _node.PublishDraining();
    }

    public void SetMaxMessageSize(long value)
    {
        _node.MaxMessageSize = value == 0 ? -1 : value;
    }

    public void SetRouterHighWaterMark(ulong value)
    {
        _node.RouterHighWaterMark = value;
    }

    public void SetRouterSendTimeout(TimeSpan? value)
    {
        _node.SendTimeout = value;
    }

    public void SetMailboxBudgets(ulong messageBudget, ulong byteBudget)
    {
        // Zero means "use the backend default". Writing zero into the managed
        // node would reject every application and infrastructure record,
        // including command 47/48 terminal completions.
        if (messageBudget != 0)
            _node.MailboxMessageBudget = messageBudget;
        if (byteBudget != 0)
            _node.MailboxByteBudget = byteBudget;
    }

    public void SetInboundDispatchBudget(ZLinkInboundDispatchBudget budget)
    {
        _node.SetInboundDispatchBudget(budget);
        _pump.SetInboundDispatchBudget(budget);
    }

    // Explicit host-startup Start (spec 21 §3): the runtime calls this after
    // routing id, bind and channels are configured, so the node is not started
    // lazily on first spot use. Idempotent; EnsureStarted stays as a defensive
    // fallback for paths that reach a spot before explicit startup.
    public void Start()
    {
        EnsureStarted();
    }

    public void ApplyRoleConfig(
        IZLinkSpotPublisherConfig? publisher,
        IZLinkSpotSubscriberConfig? subscriber)
    {
        _ = publisher;
        _ = subscriber;
    }

    public void OnSendReady(Action handler)
    {
        _pump.SetNodeSendReadyHandler(handler);
    }

    public void SetUserSpotOperationTarget(IUserSpotOperationTarget target)
    {
        _node.SetUserSpotOperationTarget(target);
    }

    public void SetActorCreateOperationTarget(IActorCreateOperationTarget target)
    {
        _node.SetActorCreateOperationTarget(target);
    }

    public void SetActorDestroyOperationTarget(IActorDestroyOperationTarget target)
    {
        _node.SetActorDestroyOperationTarget(target);
    }

    public void SetInstanceSpotActivationTarget(IInstanceSpotActivationTarget target)
    {
        _node.SetInstanceSpotActivationTarget(target);
    }

    public void SetRelocationReplyRelayTarget(IRelocationReplyRelayTarget target)
    {
        _node.SetRelocationReplyRelayTarget(target);
    }

    public ZLinkRelocationReplyCompletion TryCompleteRelocationReply(
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        IReadOnlyList<Message> payload) =>
        RequireManagedNode().TryCompleteRelocationReply(relay, payload);

    public void SetCanonicalRelocationReservationTarget(
        ICanonicalRelocationReservationTarget target)
    {
        _node.SetCanonicalRelocationReservationTarget(target);
    }

    public ValueTask<ZLinkServiceWireCodec.RelocationReservedRecord>
        ReserveCanonicalRelocationAsync(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        EnsureStarted();
        return RequireManagedNode().ReserveCanonicalRelocationAsync(
            targetNodeRid, prepare, timeout, cancellationToken);
    }

    public ValueTask StageCanonicalRelocationAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        IReadOnlyList<ZLinkServiceWireCodec.RelocationDataRecord> data,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        return RequireManagedNode().StageCanonicalRelocationAsync(
            targetNodeRid, prepare, data, timeout, cancellationToken);
    }

    public ValueTask CompleteCanonicalRelocationAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationCompleteRecord complete,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        return RequireManagedNode().CompleteCanonicalRelocationAsync(
            targetNodeRid, complete, cancellationToken);
    }

    public void CancelCanonicalRelocation(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationWireId relocationId,
        ulong targetAttemptGeneration,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator)
    {
        EnsureStarted();
        RequireManagedNode().CancelCanonicalRelocation(targetNodeRid,
            relocationId, targetAttemptGeneration, coordinator);
    }

    public ValueTask<ZLinkServiceWireCodec.ReplyRelayAckRecord>
        RelayRelocationReplyAsync(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.ReplyRelayRecord relay,
            ZLinkServiceWireCodec.RequestSourceFence expectedSource,
            IReadOnlyList<Message> payload,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        EnsureStarted();
        return RequireManagedNode().RelayRelocationReplyAsync(
            targetNodeRid,
            relay,
            expectedSource,
            payload,
            timeout,
            cancellationToken);
    }

    public async ValueTask<IReadOnlyList<Message>> ActivateInstanceSpotAsync(
        InstanceSpotActivationTarget target,
        string sourceSpotId,
        IReadOnlyList<Message> parts,
        bool request,
        ulong deadlineUnixMs,
        TimeSpan timeout,
        ReadOnlyMemory<byte> metadata,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        var submit = RequireManagedNode().ActivateInstanceSpot(
            target,
            sourceSpotId,
            parts,
            request,
            out var operationId,
            deadlineUnixMs,
            timeout,
            SendFlags.None,
            metadata);
        if (submit != SubmitResult.Ok)
            throw new ZlinkSubmitException(
                (ZlinkSubmitException.ErrorCode)(int)submit);
        if (!request) return Array.Empty<Message>();

        var terminal = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_completions.Register(operationId, (record, reply) =>
        {
            if (record.TerminalResult == (int)RequestResult.Ok)
                terminal.TrySetResult(reply);
            else
            {
                ZLinkMessageParts.DisposeAll(reply);
                terminal.TrySetException(new ZLinkFrameworkException(
                    record.FailureErrno
                    == (int)ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
                        ? ZLinkFrameworkErrorKind.InvalidOperation
                        : ZLinkFrameworkErrorKind.InternalFailure,
                    "Remote Instance Spot activation failed."));
            }
        }))
            throw new InvalidOperationException(
                "Remote Instance Spot activation did not return an operation id.");
        await using (cancellationToken.Register(
                         () => terminal.TrySetCanceled(cancellationToken))
                     .ConfigureAwait(false))
            return await terminal.Task.ConfigureAwait(false);
    }

    public ValueTask<InstanceSpotActivationTerminal> ForwardInstanceSpotActivationAsync(
        InstanceSpotActivationOperation operation,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        ReadOnlyMemory<byte>? metadata,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        return RequireManagedNode().ForwardInstanceSpotActivationAsync(
            operation,
            parts,
            metadata,
            cancellationToken);
    }

    public async ValueTask<(
        UserSpotCreateCompletion Completion,
        IReadOnlyList<Message> Reply)> CreateUserSpotAsync(
        RoutingId targetNodeRid,
        string spotId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        var submit = RequireManagedNode().CreateUserSpot(
            targetNodeRid, spotId, stableType, reservation,
            deadlineUnixMs, out var operationId, timeout);
        if (submit != SubmitResult.Ok)
            throw new ZlinkSubmitException(
                (ZlinkSubmitException.ErrorCode)(int)submit);
        var terminal = new TaskCompletionSource<(
            UserSpotCreateCompletion, IReadOnlyList<Message>)>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_completions.Register(operationId, (record, parts) =>
        {
            if (record.TerminalResult == (int)RequestResult.Ok
                && record.UserSpotCreateCompletion is { } completion)
                terminal.TrySetResult((completion, parts));
            else
            {
                ZLinkMessageParts.DisposeAll(parts);
                terminal.TrySetException(new ZLinkFrameworkException(
                    record.FailureErrno
                    == (int)ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
                        ? ZLinkFrameworkErrorKind.InvalidOperation
                        : ZLinkFrameworkErrorKind.Unavailable,
                    "Remote User Spot create failed."));
            }
        }))
            throw new InvalidOperationException(
                "Remote User Spot create did not return an operation id.");
        await using (cancellationToken.Register(
                         () => terminal.TrySetCanceled(cancellationToken))
                     .ConfigureAwait(false))
            return await terminal.Task.ConfigureAwait(false);
    }

    public async ValueTask<(
        ActorCreateCompletion Completion,
        IReadOnlyList<Message> Reply)> CreateActorRemoteAsync(
        RoutingId targetNodeRid,
        string actorId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        var submit = RequireManagedNode().CreateActorRemote(
            targetNodeRid, actorId, stableType, reservation,
            deadlineUnixMs, out var operationId, timeout);
        if (submit != SubmitResult.Ok)
            throw new ZlinkSubmitException(
                (ZlinkSubmitException.ErrorCode)(int)submit);
        var terminal = new TaskCompletionSource<(
            ActorCreateCompletion, IReadOnlyList<Message>)>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_completions.Register(operationId, (record, parts) =>
        {
            if (record.TerminalResult == (int)RequestResult.Ok
                && record.ActorCreateCompletion is { } completion)
            {
                var actor = completion.Result is ActorCreateResult.Existing
                    or ActorCreateResult.Created
                    ? completion.Actor
                    : default;
                terminal.TrySetResult((
                    completion with { Actor = actor },
                    parts));
            }
            else
            {
                ZLinkMessageParts.DisposeAll(parts);
                var failure = (ServiceWireConstants.FrameworkErrorCode)
                    record.FailureErrno;
                var kind = failure switch
                {
                    ServiceWireConstants.FrameworkErrorCode.ActorRouteNotFound =>
                        ZLinkFrameworkErrorKind.NotFound,
                    ServiceWireConstants.FrameworkErrorCode.ActorAlreadyExists =>
                        ZLinkFrameworkErrorKind.AlreadyExists,
                    ServiceWireConstants.FrameworkErrorCode.ActorTypeMismatch =>
                        ZLinkFrameworkErrorKind.TypeMismatch,
                    ServiceWireConstants.FrameworkErrorCode.ActorCreateRejected =>
                        ZLinkFrameworkErrorKind.Rejected,
                    ServiceWireConstants.FrameworkErrorCode.ActorLocationStale
                        or ServiceWireConstants.FrameworkErrorCode.RouteNotConnected
                        or ServiceWireConstants.FrameworkErrorCode.WorkerQueueFull =>
                        ZLinkFrameworkErrorKind.Unavailable,
                    ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut =>
                        ZLinkFrameworkErrorKind.DeadlineExceeded,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError =>
                        ZLinkFrameworkErrorKind.ProtocolError,
                    _ => ZLinkFrameworkErrorKind.InternalFailure
                };
                var retryAdvice = kind is
                    ZLinkFrameworkErrorKind.Unavailable
                    or ZLinkFrameworkErrorKind.DeadlineExceeded
                    ? ZLinkRetryAdvice.RetryAfterBackoff
                    : ZLinkRetryAdvice.DoNotRetry;
                terminal.TrySetException(new ZLinkFrameworkException(
                    kind,
                    $"Remote Actor create failed. result={record.TerminalResult}; "
                    + $"failure={failure}.",
                    retryAdvice));
            }
        }))
            throw new InvalidOperationException(
                "Remote Actor create did not return an operation id.");
        await using (cancellationToken.Register(
                         () => terminal.TrySetCanceled(cancellationToken))
                     .ConfigureAwait(false))
            return await terminal.Task.ConfigureAwait(false);
    }

    public async ValueTask<bool> DestroyActorRemoteAsync(
        ZLinkBackendActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        var submit = RequireManagedNode().DestroyActorRemote(
            ToNativeActor(actor),
            targetNodeGeneration,
            authorityOwnerGeneration,
            out var operationId,
            timeout);
        if (submit != SubmitResult.Ok)
            throw new ZlinkSubmitException(
                (ZlinkSubmitException.ErrorCode)(int)submit);

        var terminal = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_completions.Register(operationId, (record, parts) =>
        {
            ZLinkMessageParts.DisposeAll(parts);
            if (record.TerminalResult == (int)RequestResult.Ok
                && record.ActorDestroyCompletion is { } completion)
            {
                terminal.TrySetResult(completion.Destroyed);
                return;
            }
            terminal.TrySetException(new ZLinkFrameworkException(
                record.FailureErrno
                == (int)ServiceWireConstants.FrameworkErrorCode.ActorRouteNotFound
                    ? ZLinkFrameworkErrorKind.NotFound
                    : ZLinkFrameworkErrorKind.Unavailable,
                $"Remote Actor destroy failed for '{actor.ActorId}'.",
                record.FailureErrno
                != (int)ServiceWireConstants.FrameworkErrorCode.ActorRouteNotFound
                    ? ZLinkRetryAdvice.RetryAfterBackoff
                    : ZLinkRetryAdvice.DoNotRetry));
        }))
            throw new InvalidOperationException(
                "Remote Actor destroy did not return an operation id.");
        await using (cancellationToken.Register(
                         () => terminal.TrySetCanceled(cancellationToken))
                     .ConfigureAwait(false))
            return await terminal.Task.ConfigureAwait(false);
    }

    public async ValueTask<UserSpotCloseCompletion> CloseUserSpotAsync(
        RoutingId targetNodeRid,
        UserSpotCloseFence target,
        ulong deadlineUnixMs,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        var submit = RequireManagedNode().CloseUserSpot(
            targetNodeRid, target, deadlineUnixMs,
            out var operationId, timeout);
        if (submit != SubmitResult.Ok)
            throw new ZlinkSubmitException(
                (ZlinkSubmitException.ErrorCode)(int)submit);
        var terminal = new TaskCompletionSource<UserSpotCloseCompletion>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_completions.Register(operationId, (record, parts) =>
        {
            ZLinkMessageParts.DisposeAll(parts);
            if (record.TerminalResult == (int)RequestResult.Ok
                && record.UserSpotCloseCompletion is { } completion)
                terminal.TrySetResult(completion);
            else
                terminal.TrySetException(new ZLinkFrameworkException(
                    record.FailureErrno
                    == (int)ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
                        ? ZLinkFrameworkErrorKind.InvalidOperation
                        : ZLinkFrameworkErrorKind.Unavailable,
                    "Remote User Spot close failed: "
                    + $"result={record.TerminalResult}; "
                    + $"failure={record.FailureErrno}.",
                    record.FailureErrno
                    == (int)ServiceWireConstants.FrameworkErrorCode.SpotMoving
                        ? ZLinkRetryAdvice.RetryAfterBackoff
                        : ZLinkRetryAdvice.DoNotRetry));
        }))
            throw new InvalidOperationException(
                "Remote User Spot close did not return an operation id.");
        await using (cancellationToken.Register(
                         () => terminal.TrySetCanceled(cancellationToken))
                     .ConfigureAwait(false))
            return await terminal.Task.ConfigureAwait(false);
    }

    public void ConnectPeer(string endpoint)
    {
        _peerIntents[endpoint] = _node.ConnectPeer(endpoint);
    }

    public void ConnectPeer(
        RoutingId peerRid,
        string endpoint,
        string expectedSecurityIdentity)
    {
        _peerIntents[endpoint] = _node.ConnectPeer(
            endpoint,
            peerRid,
            expectedSecurityIdentity);
    }

    public void SetPeerExpectation(
        RoutingId peerRid,
        string endpoint,
        string expectedSecurityIdentity,
        ulong expectedLifecycleGeneration) =>
        _node.SetPeerExpectation(
            peerRid,
            endpoint,
            expectedSecurityIdentity,
            expectedLifecycleGeneration);

    public void RemovePeerExpectation(RoutingId peerRid, string endpoint) =>
        _node.RemovePeerExpectation(peerRid, endpoint);

    public void DisconnectPeer(string endpoint)
    {
        if (_peerIntents.TryRemove(endpoint, out var intent))
        {
            try
            {
                _node.RemovePeerConnection(intent);
                return;
            }
            catch (ZlinkException)
            {
                // Intent removal covers unadmitted intents only; an admitted
                // lifetime takes the exact RID+generation disconnect below.
            }
        }

        foreach (var peer in _node.Peers())
        {
            if (!string.Equals(peer.Endpoint, endpoint, StringComparison.Ordinal)
                || peer.State is not (MeshPeerState.Admitted or MeshPeerState.Draining))
                continue;
            try
            {
                _node.DisconnectPeer(peer.RoutingId, peer.LifecycleGeneration);
            }
            catch (ZlinkException)
            {
                // Core may have retired the lifetime with the transport.
            }
        }
    }

    public bool DisconnectPeerBeforeAdmission(
        RoutingId peerRid,
        string endpoint,
        ulong lifecycleGeneration)
    {
        try
        {
            var admittedPeerFound = false;
            foreach (var peer in _node.Peers())
            {
                if (!string.Equals(peer.Endpoint, endpoint, StringComparison.Ordinal)
                    || (!peer.RoutingId.IsEmpty
                        && !peerRid.IsEmpty
                        && peer.RoutingId != peerRid)
                    || (lifecycleGeneration != 0
                        && peer.LifecycleGeneration != 0
                        && peer.LifecycleGeneration != lifecycleGeneration))
                    continue;
                if (peer.State is MeshPeerState.Admitted or MeshPeerState.Draining)
                {
                    admittedPeerFound = true;
                    continue;
                }
                _node.RemovePeerConnectionIfNotAdmitted(peer.ConnectionIntentId);
            }
            // Keep the reconciler target until an admitted peer loses liveness.
            // That transition can leave a Connecting intent behind, which the
            // next tick must still remove after the descriptor has disappeared.
            return !admittedPeerFound;
        }
        catch (ZlinkException)
        {
            return false;
        }
    }

    public void DisconnectPeerLifetime(RoutingId peerRid, ulong lifecycleGeneration)
    {
        try
        {
            _node.DisconnectPeer(peerRid, lifecycleGeneration);
        }
        catch (ZlinkException)
        {
            // The lifetime may already be gone (never admitted, or Core
            // retired it with the transport); retirement is idempotent.
        }
    }

    public IZLinkBackendSpot CreateSpot()
    {
        EnsureStarted();
        return new ZLinkBackendSpotWrapper(
            _node, _node.CreateSpot(), _pump, _completions, _subscriptions);
    }

    public IZLinkBackendSpot GetOrCreateSpot(string spotId, out bool created)
    {
        EnsureStarted();
        return new ZLinkBackendSpotWrapper(
            _node, _node.GetOrCreateSpot(spotId, out created),
            _pump, _completions, _subscriptions);
    }

    public IZLinkBackendSpot GetOrCreateReservedSpot(
        string spotId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        out bool created)
    {
        EnsureStarted();
        return new ZLinkBackendSpotWrapper(
            _node,
            _node.GetOrCreateReservedSpot(
                spotId,
                objectGeneration,
                authorityOwnerGeneration,
                out created),
            _pump,
            _completions,
            _subscriptions);
    }

    public void SetLocalActorAuthority(
        ZLinkBackendActorRef actor,
        ulong authorityOwnerGeneration)
    {
        EnsureStarted();
        _node.SetActorAuthority(
            ToNativeActor(actor),
            authorityOwnerGeneration);
    }

    public ZLinkSpotNodeStatus Status()
    {
        return _node.Status().ToFramework();
    }

    public IReadOnlyList<ZLinkSpotNodePeerEntry> Peers()
    {
        var localEndpoint = _node.Status().LocalEndpoint;
        return _node.Peers()
            .SelectMany(peer =>
            {
                var channels = peer.State is MeshPeerState.Admitted
                    or MeshPeerState.Draining
                    ? _node.PeerChannels(
                        peer.RoutingId,
                        peer.LifecycleGeneration)
                    : [];
                if (channels.Length == 0)
                    return (IEnumerable<ZLinkSpotNodePeerEntry>)
                        [peer.ToFramework(localEndpoint, channel: null)];
                return channels
                    .OrderBy(static channel => channel.Name, StringComparer.Ordinal)
                    .Select(channel =>
                        peer.ToFramework(localEndpoint, channel));
            })
            .ToArray();
    }

    public MeshNodeStatus MeshStatus()
    {
        return _node.Status();
    }

    public MeshOperationId AllocateOperationId()
    {
        return _node.AllocateOperationId();
    }

    public IReadOnlyList<MeshNodePeer> MeshPeers()
    {
        return _node.Peers();
    }

    public IReadOnlyList<MeshPeerChannel> MeshPeerChannels(
        RoutingId peerRid,
        ulong lifecycleGeneration)
    {
        return _node.PeerChannels(peerRid, lifecycleGeneration);
    }

    public IMeshNodeMonitor OpenMeshMonitor(
        MeshMonitorEventMask events = MeshMonitorEventMask.All)
    {
        return _node.OpenMonitor(events);
    }

    public IReadOnlyList<ZLinkSpotNodeSubjectEntry> Subjects()
    {
        // MeshNode surfaces no subject table; the framework owns the spots it
        // created and their logical-multicast subscriptions, so the subject
        // view (spec 50 snapshot) derives from that tracking.
        return _subscriptions.Snapshot();
    }

    public IZLinkBackendSpot EntrySpot()
    {
        if (_entrySpot is { } entrySpot) return entrySpot;
        lock (_entrySpotGate)
        {
            EnsureStarted();
            return _entrySpot ??= new ZLinkBackendSpotWrapper(
                _node, _node.EntrySpot(), _pump, _completions, _subscriptions);
        }
    }

    private void EnsureStarted()
    {
        lock (_lifecycleGate)
        {
            if (!_started && !_disposed)
            {
                _started = true;
                _node.Start();
            }
        }

        _pump.EnsureStarted();
    }

    public ZLinkBackendActorRef CreateActor(string actorId, Message createRequest)
    {
        EnsureStarted();
        var actorRef = _node.CreateActor(actorId, new[] { createRequest });
        return EnsureConcreteActorRef(actorRef.ToBackend(), actorId);
    }

    public ZLinkBackendActorRef CreateReservedActor(
        string actorId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        Message createRequest)
    {
        EnsureStarted();
        var actorRef = _node.CreateReservedActor(
            actorId,
            objectGeneration,
            authorityOwnerGeneration,
            new[] { createRequest });
        return EnsureConcreteActorRef(actorRef.ToBackend(), actorId);
    }

    public ZLinkBackendActorRef? ActorLookup(string actorId)
    {
        return _node.ActorLookup(actorId, out var location)
            ? EnsureConcreteActorRef(location.Actor.ToBackend(), actorId)
            : null;
    }

    private ZLinkBackendActorRef EnsureConcreteActorRef(
        ZLinkBackendActorRef actorRef, string actorId)
    {
        if (!actorRef.NodeRid.IsEmpty) return actorRef;

        var nodeRid = _node.RoutingId;
        if (nodeRid.IsEmpty)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"Actor '{actorId}' was created on a node without a concrete routing id.");

        return actorRef with { NodeRid = nodeRid };
    }

    public bool JoinActor(
        ZLinkBackendActorRef actor,
        RoutingId destNodeRid,
        string destSpotId,
        Message message,
        RequestCallback callback,
        TimeSpan? timeout)
    {
        var operationId = _node.JoinSpot(
            ToNativeActor(actor), destNodeRid, destSpotId, 0, new[] { message },
            timeout ?? default);
        return _completions.RegisterRequest(operationId, callback);
    }

    public bool JoinActor(
        ZLinkBackendActorRef actor,
        RoutingId destNodeRid,
        string destSpotId,
        IReadOnlyList<Message> parts,
        ActorJoinCallback callback,
        TimeSpan? timeout)
    {
        var operationId = _node.JoinSpot(
            ToNativeActor(actor), destNodeRid, destSpotId, 0, parts, timeout ?? default);
        return _completions.Register(operationId, (record, replyParts) =>
            callback(BuildJoinResult(record, actor), replyParts));
    }

    public bool JoinActorEntrySpot(
        ZLinkBackendActorRef actor,
        RoutingId destNodeRid,
        Message request,
        ActorJoinEntrySpotCallback callback,
        TimeSpan? timeout)
    {
        var operationId = _node.JoinEntrySpot(
            ToNativeActor(actor), destNodeRid, new[] { request }, timeout ?? default);
        return _completions.Register(operationId, (record, replyParts) =>
            callback(BuildEntrySpotJoinResult(record, actor, destNodeRid), replyParts));
    }

    private static ZLinkBackendActorJoinResult BuildJoinResult(
        MeshReceiveRecord record, ZLinkBackendActorRef fallback)
    {
        var completion = record.JoinCompletion;
        return new ZLinkBackendActorJoinResult(
            ZLinkMeshCompletionTable.MapResult(record.TerminalResult, record.FailureErrno),
            completion is { JoinResult: ActorJoinResult.Accepted } ? 0 : 1,
            completion?.Actor.ToBackend() ?? fallback,
            completion?.Location.SpotId ?? string.Empty,
            completion?.Location.MembershipEpoch ?? 0,
            0);
    }

    private static ZLinkBackendActorJoinEntrySpotResult BuildEntrySpotJoinResult(
        MeshReceiveRecord record, ZLinkBackendActorRef fallback, RoutingId targetNodeRid)
    {
        var completion = record.JoinCompletion;
        return new ZLinkBackendActorJoinEntrySpotResult(
            ZLinkMeshCompletionTable.MapResult(record.TerminalResult, record.FailureErrno),
            completion is { JoinResult: ActorJoinResult.Accepted } ? 0 : 1,
            completion?.Actor.ToBackend() ?? fallback,
            targetNodeRid,
            completion?.Location.SpotId ?? string.Empty,
            completion?.Location.MembershipEpoch ?? 0,
            0);
    }

    public async ValueTask DestroyActorAsync(
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var operationId = _node.DestroyActor(ToNativeActor(actor), timeout);
        if (operationId == default) return;

        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _completions.Register(operationId, (_, parts) =>
        {
            ZLinkMessageParts.DisposeAll(parts);
            completion.TrySetResult();
        });
        await using (cancellationToken.Register(() => completion.TrySetCanceled())
                         .ConfigureAwait(false))
            await completion.Task.ConfigureAwait(false);
    }

    public bool SendActorBoundSession(
        ZLinkBackendActorRef actor,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        return _node.SendBoundSession(ToNativeActor(actor), parts, flags) == SubmitResult.Ok;
    }

    public SubmitResult SendToNode(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        return SendToNode(targetNodeRid, parts, flags, default);
    }

    public SubmitResult SendToNode(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        return _node.SendToNode(targetNodeRid, parts, flags, metadata);
    }

    public bool RequestToNode(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan timeout,
        ReadOnlyMemory<byte> metadata = default)
    {
        var submit = _node.RequestToNode(
            targetNodeRid,
            parts,
            callback,
            timeout,
            flags,
            metadata);
        if (submit != SubmitResult.Ok)
            return ZLinkSubmitFailureMapper.AcceptOrThrow(
                submit,
                $"node '{targetNodeRid}'");
        return true;
    }

    public SubmitResult SendToActor(
        ZLinkBackendActorRef actor,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        return _node.SendToActor(ToNativeActor(actor), parts, flags);
    }

    public async ValueTask<IReadOnlyList<Message>> RequestToActorAsync(
        ZLinkBackendActorRef actor,
        IReadOnlyList<Message> parts,
        TimeSpan? timeout,
        CancellationToken cancellationToken)
    {
        var submit = _node.RequestToActor(
            ToNativeActor(actor), parts, out var operationId, timeout ?? default);
        if (submit != SubmitResult.Ok)
            throw new ZlinkSubmitException((ZlinkSubmitException.ErrorCode)(int)submit);

        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _completions.Register(operationId, (record, replyParts) =>
        {
            var result = ZLinkMeshCompletionTable.MapResult(
                record.TerminalResult, record.FailureErrno);
            if (result == RequestResult.Ok)
            {
                completion.TrySetResult(replyParts);
                return;
            }

            ZLinkMessageParts.DisposeAll(replyParts);
            completion.TrySetException(
                new ZlinkRequestException((ZlinkRequestException.ErrorCode)(int)result));
        });
        await using (cancellationToken.Register(() => completion.TrySetCanceled())
                         .ConfigureAwait(false))
            return await completion.Task.ConfigureAwait(false);
    }

    // Relocated Actor Message Follow reply with no active binding. Spec 31 §5: once a
    // session closes, a late Actor reply is not delivered to a new session or
    // binding, and the 10.0.0 bound-session surface (spec 31 §6) provides only a
    // one-way push to the *current* binding plus close — there is no MeshNode
    // primitive that replies to an arbitrary (sourceSessionRid, requestId). In
    // 10.0.0 request replies flow through MeshOperationId completion correlation,
    // not a no-bind reply channel, so a no-bind reply is intentionally dropped.
    // Documented deviation, not a stub gap.
    public bool ReplyActorNoBind(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        IReadOnlyList<Message> parts)
    {
        // Managed direct Actor requests carry their source-owned reply route on
        // the inbound frame and are completed before this legacy backend seam.
        // Session-bound traffic continues through its binding route.
        return false;
    }

    // Forwards a relocated Actor Message Follow frame to the actor's currently bound
    // STREAM session via IMeshNode.SendBoundSession (spec 31 §6 one-way push). The
    // 9.x fine-grained (sourceNodeRid, sourceSessionRid) SNDMORE targeting has no
    // MeshNode equivalent — the target is the actor's current binding, resolved by
    // Core, not an arbitrary source session. Parts marked hasMore are buffered per
    // actor and flushed as one multipart SendBoundSession when the terminal part
    // (hasMore == false) arrives, so header+body framing is preserved. On a failed
    // flush the buffered prefix is retained so the caller's retry re-submits the
    // same multipart message without duplicating parts. Forwarding for a given
    // actor is serial (the Message Follow worker submits header then body in order).
    public bool ForwardActorBoundSessionPart(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Message message,
        bool hasMore,
        SendFlags flags)
    {
        if (hasMore)
        {
            lock (_forwardGate)
            {
                if (!_forwardBuffers.TryGetValue(actor, out var pending))
                {
                    pending = new List<Message>();
                    _forwardBuffers[actor] = pending;
                }

                pending.Add(Message.From(message));
            }

            return true;
        }

        List<Message>? buffered;
        lock (_forwardGate)
            _forwardBuffers.Remove(actor, out buffered);

        var terminal = Message.From(message);
        var parts = new List<Message>((buffered?.Count ?? 0) + 1);
        if (buffered is not null) parts.AddRange(buffered);
        parts.Add(terminal);

        // SendBoundSession clones the parts (the caller keeps ownership), so this
        // wrapper disposes every clone it owns on success.
        if (_node.SendBoundSession(ToNativeActor(actor), parts, flags) == SubmitResult.Ok)
        {
            foreach (var part in parts) part.Dispose();
            return true;
        }

        terminal.Dispose();
        if (buffered is not null)
            lock (_forwardGate)
                _forwardBuffers[actor] = buffered;
        return false;
    }

    public void CloseActorBoundSession(
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _ = _node.CloseBoundSession(ToNativeActor(actor), 0, timeout);
    }

    public void OnNodeRoute(Action<ZLinkBackendRouteReceived> handler)
    {
        _pump.SetNodeRouteHandler(handler);
    }

    public async ValueTask DisposeAsync()
    {
        if (!TryBeginDispose()) return;
        await DisposeCoreAsync(forceStop: false, CancellationToken.None)
            .ConfigureAwait(false);
    }

    public async ValueTask ForceStopAsync(CancellationToken cancellationToken)
    {
        if (!TryBeginDispose()) return;
        await DisposeCoreAsync(forceStop: true, cancellationToken)
            .ConfigureAwait(false);
    }

    private bool TryBeginDispose()
    {
        lock (_lifecycleGate)
        {
            if (_disposed) return false;
            _disposed = true;
            return true;
        }
    }

    private async Task DisposeCoreAsync(
        bool forceStop,
        CancellationToken cancellationToken)
    {
        lock (_forwardGate)
        {
            foreach (var pending in _forwardBuffers.Values)
                foreach (var part in pending)
                    part.Dispose();
            _forwardBuffers.Clear();
        }

        await _pump.DisposeAsync().ConfigureAwait(false);
        if (forceStop)
            await _node.ForceStopAsync(cancellationToken).ConfigureAwait(false);
        else
            await _node.DisposeAsync().ConfigureAwait(false);
    }

    internal sealed class ActorMessageFollowIngressAdapter(
        ZLinkMeshDispatchPump pump) : IActorMessageFollowIngressTarget
    {
        private Func<ActorMessageFollowIngress, bool>? _admission;
        private Func<IReadOnlyList<ZLinkBackendActorPart>, bool>? _handler;

        internal void SetAdmission(
            Func<ActorMessageFollowIngress, bool> admission)
        {
            ArgumentNullException.ThrowIfNull(admission);
            if (Interlocked.CompareExchange(ref _admission, admission, null)
                is { } existing
                && !ReferenceEquals(existing, admission))
                throw new InvalidOperationException(
                    "An Actor Message Follow admission handler is already registered.");
        }

        internal void SetHandler(
            Func<IReadOnlyList<ZLinkBackendActorPart>, bool> handler)
        {
            ArgumentNullException.ThrowIfNull(handler);
            if (Interlocked.CompareExchange(ref _handler, handler, null)
                is { } existing
                && !ReferenceEquals(existing, handler))
                throw new InvalidOperationException(
                    "An Actor Message Follow ingress handler is already registered.");
        }

        public bool TryFollow(ActorMessageFollowIngress ingress)
        {
            var handler = Volatile.Read(ref _handler);
            if (handler is null)
                return false;
            IReadOnlyList<Message> parts = ingress.Parts;
            var decoded = false;
            if (parts.Count == 0 && !ingress.EncodedPayload.IsEmpty)
            {
                var admission = Volatile.Read(ref _admission);
                if (admission is not null && !admission(ingress))
                    return false;
                if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                        ingress.EncodedPayload,
                        out var decodedParts))
                    return false;
                parts = decodedParts;
                decoded = true;
            }
            if (parts.Count == 0)
                return false;
            var applicationMetadata = ingress.ApplicationMetadataSource is { } source
                ? source.AsReadOnlyMemory().ToArray()
                : ingress.ApplicationMetadata;
            var actor = ingress.TargetActor.ToBackend();
            var flags = ingress.Reply is null ? 0u : 1u;
            var requestSource = pump.ResolveRequestSourceFence(
                ingress.SourceNodeRid,
                ingress.SourceNodeGeneration);
            var route = new ZLinkBackendActorRouteContext(
                ingress.OperationId,
                ingress.MessageFollowHopCount,
                ingress.TargetNodeGeneration,
                ingress.AuthorityOwnerGeneration,
                ingress.OwnerLeaseGeneration,
                ingress.ReplyRouteId,
                flags,
                DeadlineUnixMs: ingress.DeadlineUnixMs);
            var backendParts = new ZLinkBackendActorPart[parts.Count];
            for (var index = 0; index < backendParts.Length; index++)
                backendParts[index] = new ZLinkBackendActorPart(
                    actor,
                    ingress.SourceNodeRid,
                    default,
                    ingress.ReplyRouteId,
                    flags,
                    parts[index],
                    index + 1 < backendParts.Length,
                    RouteContext: route,
                    SourceNodeGeneration: ingress.SourceNodeGeneration,
                    RequestSource: requestSource,
                    DirectReply: index == 0 ? ingress.Reply : null,
                    ApplicationMetadata: applicationMetadata);
            try
            {
                var accepted = handler(backendParts);
                if (!accepted && decoded)
                    ZLinkMessageParts.DisposeAll(parts);
                return accepted;
            }
            catch
            {
                if (decoded)
                    ZLinkMessageParts.DisposeAll(parts);
                throw;
            }
        }
    }
}
