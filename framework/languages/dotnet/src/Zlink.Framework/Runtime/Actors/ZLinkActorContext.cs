namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorContext(
    ZLinkFrameworkRuntime runtime,
    ZLinkActorRuntimeState state,
    string meshName,
    ulong objectGeneration,
    string? spotId,
    IZLinkBoundSessionService boundSessionService) : IZLinkActorContext
{
    private string? _spotId = spotId;

    private IZLinkActor CurrentActor
        => state.Actor ?? throw new InvalidOperationException(
            $"Actor '{state.ActorId}' has not been created.");

    public string MeshName { get; } = meshName;

    public string ActorId => state.ActorId;

    public ulong ObjectGeneration { get; } = objectGeneration;

    public string? SpotId => Volatile.Read(ref _spotId);

    internal void UpdateSameNodeSpot(string? spotId)
    {
        state.EnsureContextValid();
        Volatile.Write(ref _spotId, spotId);
    }

    public IZLinkBoundSession BoundSession
    {
        get
        {
            state.EnsureContextValid();
            return boundSessionService.Create(state.ActorId);
        }
    }

    public IZLinkActorJoinSpotCall JoinSpot(
        string spotId,
        ZLinkMessage request)
    {
        state.EnsureContextValid();
        ArgumentNullException.ThrowIfNull(request);
        return ZLinkActorJoinCall.ForSpot(
            runtime,
            state,
            CurrentActor,
            spotId,
            request);
    }

    public IZLinkActorJoinEntrySpotCall JoinEntrySpot(ZLinkMessage request)
    {
        state.EnsureContextValid();
        ArgumentNullException.ThrowIfNull(request);
        return ZLinkActorJoinCall.ForEntrySpot(
            runtime,
            state,
            CurrentActor,
            request);
    }
}

internal sealed class ZLinkActorJoinCall :
    IZLinkActorJoinSpotCall,
    IZLinkActorJoinEntrySpotCall
{
    private readonly IZLinkActor _actor;
    private readonly ZLinkActorRuntimeState _actorState;
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly ZLinkMessage _request;
    private readonly string? _targetSpotId;
    private int _submitted;
    private TimeSpan? _timeout;

    private ZLinkActorJoinCall(
        ZLinkFrameworkRuntime runtime,
        ZLinkActorRuntimeState actorState,
        IZLinkActor actor,
        string? targetSpotId,
        ZLinkMessage request)
    {
        _runtime = runtime;
        _actorState = actorState;
        _actor = actor;
        _targetSpotId = targetSpotId;
        _request = request;
    }

    public static IZLinkActorJoinSpotCall ForSpot(
        ZLinkFrameworkRuntime runtime,
        ZLinkActorRuntimeState actorState,
        IZLinkActor actor,
        string spotId,
        ZLinkMessage request)
    {
        return new ZLinkActorJoinCall(
            runtime,
            actorState,
            actor,
            ZLinkSpotId.Require(spotId, nameof(spotId)),
            request);
    }

    public static IZLinkActorJoinEntrySpotCall ForEntrySpot(
        ZLinkFrameworkRuntime runtime,
        ZLinkActorRuntimeState actorState,
        IZLinkActor actor,
        ZLinkMessage request)
    {
        return new ZLinkActorJoinCall(
            runtime,
            actorState,
            actor,
            null,
            request);
    }

    IZLinkActorJoinSpotCall IZLinkActorJoinSpotCall.Timeout(TimeSpan timeout)
    {
        SetTimeout(timeout);
        return this;
    }

    IZLinkActorJoinEntrySpotCall IZLinkActorJoinEntrySpotCall.Timeout(TimeSpan timeout)
    {
        SetTimeout(timeout);
        return this;
    }

    public void Defer()
    {
        _actorState.EnsureContextValid();
        if (Interlocked.Exchange(ref _submitted, 1) != 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Actor Join was already deferred.");

        var snapshot = _request.Snapshot(_runtime.Registration.Codecs);
        var join = new ZLinkDeferredActorJoin(
            _runtime,
            _actorState,
            _actor,
            _actorState.NativeActorRef?.Generation
            ?? throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{_actor.Context.ActorId}' does not have a current object generation."),
            _targetSpotId,
            snapshot,
            _timeout ?? _runtime.Registration.DefaultRequestTimeout);
        ZLinkDeferredActorJoinHandlerScope.Register(
            join,
            snapshot.Encode(_runtime.Registration.Codecs).Payload.Bytes.Length);
    }

    private void SetTimeout(TimeSpan timeout)
    {
        if (Volatile.Read(ref _submitted) != 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Actor Join options cannot change after Defer.");
        ZLinkRequestTimeoutValidation.Validate(timeout, nameof(timeout));
        _timeout = timeout;
    }
}
