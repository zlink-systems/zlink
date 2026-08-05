using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkPerActorShellRelocationPlan(
    RoutingId TargetNodeRid,
    ulong TargetNodeLifecycleGeneration,
    ZLinkLocationOwnerToken TargetOwner,
    ulong TargetAuthorityOwnerGeneration,
    DateTimeOffset ClosingDeadline);

internal sealed partial class ZLinkSpotActivation :
    IZLinkSpotContext,
    IZLinkInstanceSpotContext,
    IZLinkCurrentSpotActivation,
    IZLinkSpotHandlerRegistrySink,
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
    private readonly string _spotId;
    private readonly object _lifecycleGate = new();
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

    public ZLinkSpotActivation(
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
        ZLinkSpotRelocationReadinessMode relocationReadiness =
            ZLinkSpotRelocationReadinessMode.AnyTurnBoundary,
        bool restoreLogicalTimers = false,
        ZLinkCompletionAdmissionOwner? completionAdmission = null,
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
        _spotId = ZLinkSpotId.Require(spotId, nameof(spotId));
        NodeRid = nodeRid;
        SpotNodeName = spotNodeName;
        ChannelName = channelName;
        DefaultRequestTimeout = defaultRequestTimeout;
        ExecutionMode = executionMode;
        RelocationReadiness = relocationReadiness;
        _outbound = new ZLinkSpotOutboundTransport(
            nativeSpot,
            sendTimeout,
            _stopSource.Token,
            completionAdmission);
        _outboundEndpoint = new ZLinkSpotOutboundEndpoint(
            this,
            _outbound,
            _runtime);
        Handlers = new ZLinkSpotHandlerRegistrySurface(this);
        InstanceHandlers = new ZLinkInstanceSpotHandlerRegistrySurface(this);
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
            channelName,
            _spotId,
            _packets,
            _actorJoins,
            _actors,
            _subscriptions,
            () => _actorHandlers,
            () => HandlerInvoker,
            CommitNativeActorJoinAsync,
            _outbound.Submitter,
            completionAdmission);
        _actorDispatchSubmitter = new ZLinkSpotActorDispatchSubmitter(_serial, _dispatcher.ActorPackets);
    }

    public object Spot => _spot
                              ?? throw new InvalidOperationException("SPOT has not been attached to this context.");

    internal IZLinkSpot UserSpot => Spot as IZLinkSpot
        ?? throw new InvalidOperationException("The current activation is not a User Spot.");

    public IZLinkRuntimeFailureReporter ErrorSink => _runtime.ErrorSink;

    private ZLinkSpotHandlerInvoker HandlerInvoker => _handlerInvoker
                                                      ?? throw new InvalidOperationException(
                                                          "SPOT has not been attached to this context.");

    public IZLinkBackendSpot NativeSpot { get; }

    public string SpotNodeName { get; }

    public int JoinedActorCount => _actors.Count;

    internal bool ContainsActor(string actorId) =>
        _actors.TryGetActor(actorId, out _);

    public bool IsDisposed => Volatile.Read(ref _disposed) != 0;

    public string ChannelName { get; }

    public string MeshName => ChannelName;

    public TimeSpan DefaultRequestTimeout { get; }

    public ZLinkUserSpotExecutionMode ExecutionMode { get; }

    internal ZLinkSpotRelocationReadinessMode RelocationReadiness { get; }

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

    public IZLinkSpotHandlerRegistry Handlers { get; }

    private IZLinkInstanceSpotHandlerRegistry InstanceHandlers { get; }

    IZLinkInstanceSpotHandlerRegistry IZLinkInstanceSpotContext.Handlers => InstanceHandlers;

    public IZLinkSpotRelocationReadyCall RelocationReady()
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

    public string SpotId => _spotId;

    public ulong ObjectGeneration => NativeSpot.LifecycleGeneration;

    public RoutingId NodeRid { get; }

    private void EnsureContextOperationAllowed()
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
}
