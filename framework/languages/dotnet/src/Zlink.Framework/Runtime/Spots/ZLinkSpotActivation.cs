using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkPerActorShellRelocationPlan(
    RoutingId TargetNodeRid,
    ulong TargetNodeLifecycleGeneration,
    ZLinkLocationOwnerToken TargetOwner,
    ulong TargetAuthorityOwnerGeneration,
    DateTimeOffset ClosingDeadline);

internal abstract partial class ZLinkSpotActivation :
    IZLinkCurrentSpotActivation,
    IZLinkInstanceSpotHandlerRegistrySink,
    IAsyncDisposable
{
    private readonly ZLinkSpotActorDispatchSubmitter _actorDispatchSubmitter;
    private readonly ZLinkSpotActorJoinRegistry _actorJoins = new();
    private readonly ZLinkSpotActorMembership _actors = new();
    private readonly TaskCompletionSource _perActorMembersDrained =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly ZLinkSpotActivationDispatcher _dispatcher;
    private readonly ZLinkSpotOutboundTransport _outbound;
    private readonly ZLinkSpotOutboundEndpoint _outboundEndpoint;
    private readonly ZLinkSpotPacketRegistry _packets = new();
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly ZLinkScopedHandlerInstanceOwner _handlerInstances;
    private readonly AsyncServiceScope _scope;
    private readonly ZLinkSpotSerialExecutor _serial;
    private readonly CancellationTokenSource _stopSource = new();
    private readonly ZLinkSpotSubscriptionRegistry _subscriptions = new();
    private readonly ZLinkSpotTimerRegistry _timers;
    private readonly ZLinkSpotId _spotId;
    private readonly ZLinkStateLane _lane = new();
    private readonly SemaphoreSlim _membershipPublicationGate = new(1, 1);
    private ZLinkSpotActorHandlerRegistry? _actorHandlers;
    private bool _configurationOpen = true;
    private int _nativeDispatchAttached;
    private int _disposed;
    private int _closingInvoked;
    private ZLinkSpotHandlerInvoker? _handlerInvoker;
    private Task? _finalization;
    private object? _spot;
    private ZLinkPerActorShellRelocationPlan? _perActorShellRelocation;

    protected ZLinkSpotActivation(
        ZLinkFrameworkRuntime runtime,
        AsyncServiceScope scope,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        RoutingId nodeRid,
        string spotNodeName,
        string channelName,
        TimeSpan defaultRequestTimeout,
        TimeSpan? sendTimeout,
        ZLinkUserSpotExecutionMode executionMode = ZLinkUserSpotExecutionMode.SpotWide,
        ZLinkSpotRelocationCoordinationMode relocationCoordinationMode =
            ZLinkSpotRelocationCoordinationMode.FrameworkManaged,
        bool restoreLogicalTimers = false,
        ZLinkTimerScheduler? timerScheduler = null)
    {
        _runtime = runtime;
        _timers = new ZLinkSpotTimerRegistry(
            () => runtime.Flow.CaptureEnabled,
            restoreLogicalTimers,
            timerScheduler);
        _scope = scope;
        _handlerInstances = new ZLinkScopedHandlerInstanceOwner(scope.ServiceProvider);
        NativeSpot = nativeSpot;
        _spotId = ZLinkSpotId.FromBoundary(spotId, nameof(spotId));
        NodeRid = nodeRid;
        SpotNodeName = spotNodeName;
        RuntimeChannelName = ZLinkChannelName.FromBoundary(
            channelName,
            nameof(channelName));
        RuntimeMeshName = ZLinkMeshName.FromBoundary(
            channelName,
            nameof(channelName));
        DefaultRequestTimeout = defaultRequestTimeout;
        ExecutionMode = executionMode;
        RelocationCoordinationMode = relocationCoordinationMode;
        _outbound = new ZLinkSpotOutboundTransport(
            nativeSpot,
            sendTimeout,
            _stopSource.Token);
        _outboundEndpoint = new ZLinkSpotOutboundEndpoint(
            this,
            _outbound,
            _runtime);
        _serial = new ZLinkSpotSerialExecutor(
            this,
            () => IsDisposed,
            _stopSource.Token,
            runtime.ErrorSink,
            () => runtime.Flow.CaptureEnabled,
            executionMode: executionMode);
        _dispatcher = new ZLinkSpotActivationDispatcher(
            runtime,
            nativeSpot,
            RuntimeChannelName.Value,
            _spotId.Value,
            _packets,
            _actorJoins,
            _actors,
            _subscriptions,
            () => _actorHandlers,
            () => HandlerInvoker,
            CommitNativeActorJoinAsync);
        _actorDispatchSubmitter = new ZLinkSpotActorDispatchSubmitter(_serial, _dispatcher.ActorPackets);
    }

    public object Spot => _spot
                              ?? throw new InvalidOperationException("SPOT has not been attached to this context.");

    internal abstract ZLinkSpotKind SpotKind { get; }

    internal abstract ZLinkPlacementObjectKind PlacementKind { get; }

    internal abstract string KindName { get; }

    internal abstract bool SupportsIdleEviction { get; }

    internal IZLinkSpot UserSpot => Spot as IZLinkSpot
        ?? throw new InvalidOperationException("The current activation is not a User Spot.");

    internal IZLinkInstanceSpot InstanceSpot => Spot as IZLinkInstanceSpot
        ?? throw new InvalidOperationException("The current activation is not an Instance Spot.");

    public IZLinkRuntimeFailureReporter ErrorSink => _runtime.ErrorSink;

    private ZLinkSpotHandlerInvoker HandlerInvoker => _handlerInvoker
                                                      ?? throw new InvalidOperationException(
                                                          "SPOT has not been attached to this context.");

    public IZLinkBackendSpot NativeSpot { get; }

    public string SpotNodeName { get; }

    public int JoinedActorCount => _actors.Count;

    internal bool ContainsActor(string actorId) =>
        _actors.TryGetActor(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            out _);

    public bool IsDisposed => Volatile.Read(ref _disposed) != 0;

    public string ChannelName => RuntimeChannelName.Value;

    internal ZLinkChannelName RuntimeChannelName { get; }

    public string MeshName => RuntimeMeshName.Value;

    internal ZLinkMeshName RuntimeMeshName { get; }

    public TimeSpan DefaultRequestTimeout { get; }

    public ZLinkUserSpotExecutionMode ExecutionMode { get; }

    internal ZLinkSpotRelocationCoordinationMode RelocationCoordinationMode { get; }

    internal ZLinkPerActorShellRelocationPlan?
        PerActorShellRelocationPlan =>
        Volatile.Read(ref _perActorShellRelocation);

    internal async ValueTask PublishPerActorShellRelocationPlanAsync(
        ZLinkPerActorShellRelocationPlan plan,
        CancellationToken cancellationToken = default)
    {
        if (ExecutionMode != ZLinkUserSpotExecutionMode.PerActor)
            throw new InvalidOperationException(
                "Only a PerActor User Spot can publish a shell relocation plan.");
        await _membershipPublicationGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            if (Interlocked.CompareExchange(
                    ref _perActorShellRelocation,
                    plan,
                    null) is { } existing
                && existing != plan)
                throw new ZLinkRelocationDataLostException(
                    $"SPOT '{SpotId}' shell relocation target changed.");
            if (JoinedActorCount == 0)
                _perActorMembersDrained.TrySetResult();
        }
        finally
        {
            _membershipPublicationGate.Release();
        }
    }

    internal Task WaitForPerActorMembersDrainedAsync(
        CancellationToken cancellationToken)
    {
        if (ExecutionMode != ZLinkUserSpotExecutionMode.PerActor
            || PerActorShellRelocationPlan is null)
            throw new InvalidOperationException(
                "Only a relocated PerActor User Spot can wait for member relocation.");
        if (JoinedActorCount == 0)
            _perActorMembersDrained.TrySetResult();
        return _perActorMembersDrained.Task.WaitAsync(cancellationToken);
    }

    public ZLinkCodecRegistryBuilder Codecs => _runtime.Registration.Codecs;

    ZLinkMessageFlowTracer IZLinkCurrentSpotActivation.Flow => _runtime.Flow;

    protected IZLinkSpotRelocationReadyCall CreateRelocationReadyCall()
    {
        EnsureContextOperationAllowed();
        return new ZLinkSpotRelocationReadyCall(this);
    }

    public IZLinkSpotOutbound Outbound
    {
        get
        {
            EnsureContextOperationAllowed();
            return _outboundEndpoint;
        }
    }

    ZLinkSpotOutboundEndpoint IZLinkCurrentSpotActivation.OutboundEndpoint => _outboundEndpoint;

    void IZLinkCurrentSpotActivation.EnsureOperationAllowed() =>
        EnsureContextOperationAllowed();

    public string SpotId => _spotId.Value;

    internal ZLinkSpotId RuntimeSpotId => _spotId;

    public ulong ObjectGeneration => NativeSpot.LifecycleGeneration;

    public RoutingId NodeRid { get; }

    protected void EnsureContextOperationAllowed()
    {
        if (IsDisposed)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Spot context for '{SpotId}' cannot start an operation after owner cutover.");
        ZLinkSpotRelocationReadyHandlerScope.EnsureOperationCanStart(this);
    }

    private void EnsureConfigurationOpen()
    {
        if (!_configurationOpen)
            throw new InvalidOperationException(
                "SPOT handler registration is only allowed while IZLinkSpot.Configure is running.");
    }

    private ValueTask CommitNativeActorJoinAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        return CommitActorJoinCoreAsync(actor, cancellationToken);
    }

    void IZLinkInstanceSpotHandlerRegistrySink.AddPacket<THandler>() =>
        AddPacketCore<THandler>();
}

internal sealed class ZLinkUserSpotActivation :
    ZLinkSpotActivation,
    IZLinkSpotContext,
    IZLinkSpotHandlerRegistrySink
{
    internal ZLinkUserSpotActivation(
        ZLinkFrameworkRuntime runtime,
        AsyncServiceScope scope,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        RoutingId nodeRid,
        string spotNodeName,
        string channelName,
        TimeSpan defaultRequestTimeout,
        TimeSpan? sendTimeout,
        ZLinkUserSpotExecutionMode executionMode = ZLinkUserSpotExecutionMode.SpotWide,
        ZLinkSpotRelocationCoordinationMode relocationCoordinationMode =
            ZLinkSpotRelocationCoordinationMode.FrameworkManaged,
        bool restoreLogicalTimers = false,
        ZLinkTimerScheduler? timerScheduler = null)
        : base(
            runtime,
            scope,
            nativeSpot,
            spotId,
            nodeRid,
            spotNodeName,
            channelName,
            defaultRequestTimeout,
            sendTimeout,
            executionMode,
            relocationCoordinationMode,
            restoreLogicalTimers,
            timerScheduler)
    {
        Handlers = new ZLinkSpotHandlerRegistrySurface(this);
    }

    internal override ZLinkSpotKind SpotKind => ZLinkSpotKind.User;

    internal override ZLinkPlacementObjectKind PlacementKind =>
        ZLinkPlacementObjectKind.UserSpot;

    internal override string KindName => "user";

    internal override bool SupportsIdleEviction => false;

    protected override ValueTask BindKindDescriptorsAsync(
        CancellationToken cancellationToken) =>
        BindUserDescriptorsAsync(cancellationToken);

    protected override void ValidateScannedHandlerKind(
        ZLinkScannedSpotHandlerKind kind)
    {
        _ = kind;
    }

    protected override ValueTask InvokeClosingAsync(
        ZLinkSpotCloseReason reason,
        DateTimeOffset deadline) =>
        ZLinkSpotClosingInvocation.InvokeAsync(
            UserSpot.OnClosingAsync,
            reason,
            deadline);

    public IZLinkSpotHandlerRegistry Handlers { get; }

    internal void AttachSpot(IZLinkSpot spot) => AttachUserSpotCore(spot);

    public IZLinkSpotRelocationReadyCall RelocationReady() =>
        CreateRelocationReadyCall();

    ValueTask IZLinkSpotContext.LeaveActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken) =>
        LeaveActorFromContextAsync(actor, cancellationToken);

    ValueTask<bool> IZLinkSpotContext.CloseAsync(CancellationToken cancellationToken) =>
        CloseFromContextAsync(cancellationToken);

    void IZLinkSpotHandlerRegistrySink.AddSubscribe<THandler>(
        string channelName,
        string topic) =>
        AddSubscribeCore<THandler>(channelName, topic);

    void IZLinkSpotHandlerRegistrySink.AddHandler<THandler>() =>
        AddHandlerCore<THandler>();

    void IZLinkSpotHandlerRegistrySink.AddHandler<THandler>(string packetName) =>
        AddHandlerCore<THandler>(packetName);

    void IZLinkSpotHandlerRegistrySink.AddActorPacket<THandler, TActor>() =>
        AddActorPacketCore<THandler, TActor>();

    void IZLinkSpotHandlerRegistrySink.AddActorPacket<THandler, TActor>(
        string packetName) =>
        AddActorPacketCore<THandler, TActor>(packetName);
}

internal sealed class ZLinkInstanceSpotActivation :
    ZLinkSpotActivation,
    IZLinkInstanceSpotContext
{
    internal ZLinkInstanceSpotActivation(
        ZLinkFrameworkRuntime runtime,
        AsyncServiceScope scope,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        RoutingId nodeRid,
        string spotNodeName,
        string channelName,
        TimeSpan defaultRequestTimeout,
        TimeSpan? sendTimeout,
        bool restoreLogicalTimers = false,
        ZLinkTimerScheduler? timerScheduler = null)
        : base(
            runtime,
            scope,
            nativeSpot,
            spotId,
            nodeRid,
            spotNodeName,
            channelName,
            defaultRequestTimeout,
            sendTimeout,
            restoreLogicalTimers: restoreLogicalTimers,
            timerScheduler: timerScheduler)
    {
        Handlers = new ZLinkInstanceSpotHandlerRegistrySurface(this);
    }

    internal override ZLinkSpotKind SpotKind => ZLinkSpotKind.Instance;

    internal override ZLinkPlacementObjectKind PlacementKind =>
        ZLinkPlacementObjectKind.InstanceSpot;

    internal override string KindName => "instance";

    internal override bool SupportsIdleEviction => true;

    protected override ValueTask BindKindDescriptorsAsync(
        CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }

    protected override void ValidateScannedHandlerKind(
        ZLinkScannedSpotHandlerKind kind)
    {
        if (kind is ZLinkScannedSpotHandlerKind.Subscription
            or ZLinkScannedSpotHandlerKind.ActorSend
            or ZLinkScannedSpotHandlerKind.ActorRequest)
            throw new ZLinkConfigurationException(
                $"Instance Spot '{Spot.GetType()}' cannot register {kind} handlers.");
    }

    protected override ValueTask InvokeClosingAsync(
        ZLinkSpotCloseReason reason,
        DateTimeOffset deadline) =>
        ZLinkSpotClosingInvocation.InvokeAsync(
            InstanceSpot.OnClosingAsync,
            reason,
            deadline);

    internal ValueTask InitializeAsync(CancellationToken cancellationToken) =>
        InitializeInstanceCoreAsync(cancellationToken);

    public IZLinkInstanceSpotHandlerRegistry Handlers { get; }

    internal void AttachInstanceSpot(IZLinkInstanceSpot spot) =>
        AttachInstanceSpotCore(spot);

    ValueTask<bool> IZLinkInstanceSpotContext.CloseAsync(
        CancellationToken cancellationToken) =>
        CloseFromContextAsync(cancellationToken);
}
