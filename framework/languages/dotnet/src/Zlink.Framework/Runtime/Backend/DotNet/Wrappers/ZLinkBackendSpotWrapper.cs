using Zlink.Framework.Runtime.Backend.DotNet.Mappings;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// RouteMesh 10.0.0 spot seam over the binding ISpot plus the node dispatch pump.
// Inbound route/subscribe/actor-join/lifecycle records are pulled from the pump's
// per-spot queues (fed by the node DrainReady loop); outbound requests register a
// completion in the node completion table.
internal sealed class ZLinkBackendSpotWrapper :
    IZLinkBackendSpot,
    IZLinkBackendSpotMessageFollower,
    IZLinkBackendAuthorityObserver
{
    private readonly IMeshNode _node;
    private readonly ISpot _spot;
    private readonly ZLinkMeshDispatchPump _pump;
    private readonly ZLinkMeshCompletionTable _completions;
    private readonly ZLinkMeshDispatchPump.SpotDispatchState _state;

    public ZLinkBackendSpotWrapper(
        IMeshNode node,
        ISpot spot,
        ZLinkMeshDispatchPump pump,
        ZLinkMeshCompletionTable completions,
        ZLinkSpotSubscriptionTracker? subscriptions = null)
    {
        _node = node;
        _spot = spot;
        _pump = pump;
        _completions = completions;
        _subscriptions = subscriptions;
        _state = pump.RegisterSpot(ZLinkSpotId.FromNativeRoutingId(spot.RoutingId));
    }

    private readonly ZLinkSpotSubscriptionTracker? _subscriptions;

    public RoutingId RoutingId => _spot.RoutingId;

    private string SpotId => ZLinkSpotId.FromNativeRoutingId(_spot.RoutingId);

    public ulong LifecycleGeneration => _spot.Status().LifecycleGeneration;

    public void SetLocalOwnerLeaseGeneration(ulong ownerLeaseGeneration) =>
        RequireManagedNode().SetLocalOwnerLeaseGeneration(ownerLeaseGeneration);

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

    private ZLinkManagedMeshNode RequireManagedNode() =>
        _node as ZLinkManagedMeshNode
        ?? throw new InvalidOperationException(
            "Authority fencing requires the Framework managed MeshNode.");

    private ActorRef ToNativeActor(ZLinkBackendActorRef actor) =>
        actor.ToNative(RequireManagedNode().MeshName);

    internal ISpot NativeSpot => _spot;

    // Entry Spot identity is assigned after the MeshNode creates its reserved
    // node-rid key. The managed ISpot owns the catalog rekey, so the backend
    // wrapper must forward this lifecycle operation instead of dropping it.
    public void SetRoutingId(RoutingId routingId)
    {
        var previousSpotId = SpotId;
        var currentSpotId = ZLinkSpotId.FromNativeRoutingId(routingId);
        ZLinkSpotId.Require(currentSpotId, nameof(routingId));
        _pump.RekeySpot(previousSpotId, currentSpotId, _state);
        try
        {
            _spot.SetRoutingId(routingId);
        }
        catch
        {
            _pump.RekeySpot(currentSpotId, previousSpotId, _state);
            throw;
        }
    }

    public void SetSubscription(string channelName, string topic)
    {
        _spot.SetSubscription(channelName, topic);
        _subscriptions?.Add(SpotId, channelName, topic);
    }

    public ZLinkBackendSubscribeMessage? Subscribe(RecvFlags flags)
    {
        return _state.Subscriptions.TryDequeue(out var message) ? message : null;
    }

    public ZLinkBackendRouteReceived? RecvRoute(RecvFlags flags)
    {
        return _state.Routes.TryDequeue(out var route) ? route : null;
    }

    public void OnDispatchEvent(Action<ZLinkBackendSpotDispatchInfo> handler)
    {
        _pump.SetDispatchHandler(SpotId, handler);
    }

    public void OnSendReady(Action handler)
    {
        _state.SendReadyHandler = handler;
    }

    public bool RequestToChannel(
        string channelName,
        Message message,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata)
    {
        return RequestToChannel(
            channelName, new[] { message }, callback, flags, timeout, metadata);
    }

    public bool RequestToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata)
    {
        var submit = _spot.RequestToChannel(
            channelName, parts, callback, timeout ?? default, flags,
            metadata);
        return AcceptChannelRequestSubmit(submit, $"channel '{channelName}'");
    }

    public SubmitResult SendToChannel(
        string channelName, Message message, SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        return _spot.SendToChannel(channelName, new[] { message }, flags, metadata);
    }

    public SubmitResult SendToChannel(
        string channelName, IReadOnlyList<Message> parts, SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        return _spot.SendToChannel(channelName, parts, flags, metadata);
    }

    public void Publish(
        string channelName, string topic, Message message, SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        _spot.Publish(channelName, topic, new[] { message }, flags, metadata);
    }

    public void Publish(
        string channelName, string topic, IReadOnlyList<Message> parts, SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        _spot.Publish(channelName, topic, parts, flags, metadata);
    }

    public SubmitResult SendToSpot(
        RoutingId targetRid, string spotId, ulong spotGeneration,
        Message message, SendFlags flags, ReadOnlyMemory<byte> metadata)
    {
        return _spot.SendToSpot(
            targetRid, spotId, spotGeneration, new[] { message }, flags, metadata);
    }

    public SubmitResult SendToSpot(
        RoutingId targetRid, string spotId, ulong spotGeneration,
        IReadOnlyList<Message> parts, SendFlags flags, ReadOnlyMemory<byte> metadata)
    {
        return _spot.SendToSpot(
            targetRid, spotId, spotGeneration, parts, flags, metadata);
    }

    public bool RequestToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        Message message,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata)
    {
        return RequestToSpot(
            targetRid, spotId, spotGeneration, new[] { message }, callback, flags,
            timeout, metadata);
    }

    public bool RequestToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata)
    {
        var submit = _spot.RequestToSpot(
            targetRid, spotId, spotGeneration, parts, out var operationId,
            timeout ?? default, flags, metadata);
        return AcceptRequestSubmit(submit, $"SPOT '{spotId}' on node '{targetRid}'")
               && _completions.RegisterRequest(operationId, callback);
    }

    public SubmitResult MessageFollowSendToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        MeshOperationId operationId,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata) =>
        RequireManagedNode().MessageFollowSendToSpot(
            SpotId,
            targetRid,
            spotId,
            spotGeneration,
            operationId,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            messageFollowHopCount,
            parts,
            flags,
            metadata);

    public bool MessageFollowRequestToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        MeshOperationId operationId,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        ulong deadlineUnixMs,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata)
    {
        var submit = RequireManagedNode().MessageFollowRequestToSpot(
            SpotId,
            targetRid,
            spotId,
            spotGeneration,
            operationId,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            messageFollowHopCount,
            deadlineUnixMs,
            parts,
            out var transportOperationId,
            timeout ?? default,
            flags,
            metadata);
        return AcceptRequestSubmit(
                   submit,
                   $"Message Follow for Spot '{spotId}' on node '{targetRid}'")
               && _completions.RegisterRequest(
                   transportOperationId,
                   callback);
    }

    // Terminal admission failures (NotFound, InvalidState, ...) surface to the
    // caller; Backpressured waits for send-ready; a NotConnected admission gap
    // is a retriable transport window and rides the async submitter's retry
    // classification instead of failing the blocking call outright.
    private static bool AcceptRequestSubmit(SubmitResult submit, string targetDescription)
    {
        return submit switch
        {
            SubmitResult.Ok => true,
            SubmitResult.Backpressured => false,
            SubmitResult.NotConnected =>
                throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected),
            _ => throw ZLinkSubmitFailureMapper.CreateException(submit, targetDescription)
        };
    }

    private static bool AcceptChannelRequestSubmit(
        SubmitResult submit,
        string targetDescription)
    {
        return submit switch
        {
            SubmitResult.Ok => true,
            SubmitResult.Backpressured => false,
            SubmitResult.NotConnected =>
                throw ZLinkSubmitFailureMapper.CreateChannelException(
                    submit,
                    targetDescription),
            _ => throw ZLinkSubmitFailureMapper.CreateChannelException(
                submit,
                targetDescription)
        };
    }

    public ZLinkBackendActorJoinRequest? RecvActorJoin(RecvFlags flags)
    {
        return _state.ActorJoins.TryDequeue(out var request) ? request : null;
    }

    public ZLinkBackendSpotActorLifecycleEvent? RecvActorLifecycle(RecvFlags flags)
    {
        return _state.Lifecycles.TryDequeue(out var lifecycle) ? lifecycle : null;
    }

    public void ReplyActorJoin(
        ZLinkBackendActorJoinRequest request, int joinResultCode, Message reply)
    {
        RequireMeshRequest(request).ReplyJoin(joinResultCode, new[] { reply });
    }

    public void ReplyActorJoin(
        ZLinkBackendActorJoinRequest request,
        int joinResultCode,
        IReadOnlyList<Message> parts)
    {
        RequireMeshRequest(request).ReplyJoin(joinResultCode, parts);
    }

    private static ZLinkMeshActorJoinRequest RequireMeshRequest(
        ZLinkBackendActorJoinRequest request) =>
        request as ZLinkMeshActorJoinRequest
        ?? throw new InvalidOperationException("Expected a MeshNode actor join request.");

    public ValueTask DisposeAsync()
    {
        _subscriptions?.RemoveSpot(SpotId);
        return _spot.DisposeAsync();
    }
}
