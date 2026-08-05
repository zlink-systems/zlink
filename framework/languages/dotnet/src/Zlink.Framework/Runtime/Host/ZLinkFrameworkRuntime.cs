using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Actors;

namespace Zlink.Framework.Runtime.Host;

internal interface IZLinkRuntimeTerminalFailureSink
{
    void SealError(Exception error);
}

internal readonly record struct CreateActorResult(
    IZLinkActor Actor,
    bool Created,
    ZLinkMessage CreateRequest,
    ZLinkActorCreateResponse? Response = null);

internal readonly record struct ZLinkDrainRemainderCounts(
    int Actors,
    int Spots,
    int Requests,
    int Sessions);

internal readonly record struct ZLinkRuntimeOperationAdmissionSnapshot(
    long Epoch,
    int ActiveCount);

internal readonly record struct ZLinkRelocationAdmissionFence(long Generation);

internal sealed partial class ZLinkFrameworkRuntime : IZLinkSpotManager
{
    private static readonly AsyncLocal<ZLinkRuntimeOperationOwnership?> AmbientOperation = new();
    private readonly ZLinkActorDrainCoordinator _actorDrainCoordinator;
    private readonly ZLinkStandaloneActorRelocationRuntime
        _standaloneActorRelocationRuntime;
    private readonly ZLinkActorBoundSessionCoordinator _actorBoundSessionCoordinator;
    private readonly ZLinkFrameworkActorFacade _actors;
    private readonly ZLinkActorSessionManager _actorSessionManager;
    private readonly ZLinkActorHandoffAdmissions _actorHandoffAdmissions;
    private readonly IZLinkBackendAdapterFactory _backendAdapterFactory;
    private readonly ZLinkLocationAutoConnectHost? _autoConnect;
    private readonly ZLinkChannelRuntimeManager _channels;
    private readonly ZLinkDrainAdmissionGate _drainAdmission;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private ZLinkRuntimeExecutionScope? _executionScope;
    private readonly object _operationGate = new();
    private readonly ZLinkLocationLifecycle? _locationLifecycle;
    private readonly ZLinkLocationRuntime? _locationRuntime;
    private readonly ZLinkRelocationPermitPool _relocationPermits;
    private readonly ZLinkRelocationInterruptionObserver
        _relocationInterruption;
    private readonly IZLinkAutoConnectTopologyQuery? _topologyQuery;
    private readonly ZLinkSpotRouteRouterDispatcher _spotRouteRouter;
    private readonly ZLinkSpotRuntimeManager _spots;
    private readonly ZLinkFrameworkComponentStateFactory _stateFactory;
    private readonly ZLinkStreamRuntimeManager _streams;
    private readonly object _workerPoolGate = new();
    private ZLinkMessageFlowTracer? _flow;
    private ILogger? _actorHandoffLogger;
    private ILogger? _timerLogger;
    private ZLinkRuntimeErrorSink? _generationErrorSink = new();
    private int _lifecyclePhase;
    private ZLinkFrameworkComponentState? _state;
    private ZLinkWorkerPool? _workerPool;
    private ZLinkWorkerPool? _logicalMulticastWorkerPool;
    private TaskCompletionSource? _operationsDrained;
    private TaskCompletionSource? _operationsAtZero;
    private int _activeOperations;
    private long _operationEpoch;
    private int _activeRequests;
    private bool _acceptingOperations;
    private ZLinkDrainOwner _admissionOwner;
    private long _nextRelocationFenceGeneration;
    private long _relocationFenceGeneration;
    private long _nextInstanceActivationSelection;
    private ZLinkRelocationTargetSelection _relocationTargetSelection;

    public ZLinkFrameworkRuntime(
        IServiceProvider services,
        IZLinkBackendAdapterFactory backendAdapterFactory,
        ZLinkFrameworkRegistration registration,
        ZLinkHandlerRegistry handlerRegistry,
        ZLinkHandlerDispatcher dispatcher)
    {
        Services = services;
        _actorHandoffAdmissions = new ZLinkActorHandoffAdmissions(
            diagnostic: LogActorHandoff,
            abortCapacityReservation:
                AbortActorHandoffCapacityReservationAsync);
        _backendAdapterFactory = backendAdapterFactory;
        _autoConnect = services.GetService<ZLinkLocationAutoConnectHost>();
        Registration = registration;
        _drainAdmission = services.GetService<ZLinkDrainAdmissionGate>() ?? new ZLinkDrainAdmissionGate();
        _locationLifecycle = services.GetService<ZLinkLocationLifecycle>();
        _locationRuntime = services.GetService<ZLinkLocationRuntime>();
        _topologyQuery = services.GetService<IZLinkAutoConnectTopologyQuery>();
        _relocationPermits = new ZLinkRelocationPermitPool(
            registration.Locations.Options);
        _relocationInterruption =
            new ZLinkRelocationInterruptionObserver(
                services.GetService<ILoggerFactory>());
        var components = ZLinkFrameworkRuntimeComponentFactory.Create(
            this,
            services,
            backendAdapterFactory,
            registration,
            _locationLifecycle,
            handlerRegistry,
            dispatcher,
            GetOrStartState,
            GetActorSpotNode,
            actorType => GetActorSpotNodeRuntime(actorType)?.ActivationAdmission);
        _channels = components.Channels;
        _streams = components.Streams;
        _spots = components.Spots;
        _stateFactory = components.StateFactory;
        _actorSessionManager = components.ActorSessionManager;
        _actors = components.Actors;
        _actorBoundSessionCoordinator = new ZLinkActorBoundSessionCoordinator(
            _actorSessionManager.GetOrCreateState,
            () => GetActorSpotNode() ?? GetRouterSpotNodeOrNull(),
            meshName => GetMeshNodeRuntime(meshName).Node,
            registration,
            () => ShutdownToken)
        {
            RemotePushRelay = RelayRemoteSessionPush,
            RemoteFrameRelay = RelayRemoteActorFrame
        };
        _standaloneActorRelocationRuntime =
            new ZLinkStandaloneActorRelocationRuntime(
                this,
                _actorSessionManager,
                registration);
        _actorDrainCoordinator = new ZLinkActorDrainCoordinator(
            _standaloneActorRelocationRuntime,
            _actorSessionManager,
            services,
            registration);
        _actorMessageFollower = new ZLinkActorMessageFollower(this);
        _spotRouteRouter = new ZLinkSpotRouteRouterDispatcher(GetOrStartState);
    }

    public IZLinkBackendContext? Context
        => Volatile.Read(ref _lifecyclePhase) == (int)ZLinkRuntimeLifecyclePhase.Running
            ? _state?.Context
            : null;

    public ZLinkFrameworkRegistration Registration { get; }

    internal ZLinkStandaloneActorRelocationRuntime
        StandaloneActorRelocationRuntime => _standaloneActorRelocationRuntime;

    // Shared success-path tracer for outbound client calls (channel/route/spot/actor
    // send/request/publish), built once. Inbound surfaces use the reporter's Flow.
    internal ZLinkMessageFlowTracer Flow => _flow ??= new ZLinkMessageFlowTracer(
        Registration.DispatchOptions,
        ZLinkMessageFlowTracer.CreateLogger(Services.GetService<ILoggerFactory>()),
        this);

    internal void LogActorHandoff(string marker)
    {
        _actorHandoffLogger ??= Services.GetService<ILoggerFactory>()?
            .CreateLogger("Zlink.Framework.ActorHandoff");
        _actorHandoffLogger?.LogInformation("{ActorHandoffMarker}", marker);
    }

    internal ZLinkRuntimeErrorSink ErrorSink => Volatile.Read(ref _generationErrorSink)
                                                 ?? throw new InvalidOperationException(
                                                     "The framework runtime error sink is not active.");

    internal ZLinkRuntimeErrorSink PrepareErrorSink()
    {
        var current = Volatile.Read(ref _generationErrorSink);
        if (current is not null) return current;
        var created = new ZLinkRuntimeErrorSink();
        return Interlocked.CompareExchange(ref _generationErrorSink, created, null) ?? created;
    }

    internal void DetachErrorSink(ZLinkRuntimeErrorSink errorSink) =>
        Interlocked.CompareExchange(ref _generationErrorSink, null, errorSink);

    internal void TryReportUnhandledCallbackException(Exception exception) =>
        Volatile.Read(ref _generationErrorSink)?.ReportUnhandledCallbackException(exception);

    internal void ReportTimerFailure(
        string sourceName,
        string spotId,
        bool isEntrySpot,
        string timerName,
        Type handlerType,
        ZLinkTimerTick tick,
        Exception exception,
        bool stopped)
    {
        TryReportUnhandledCallbackException(exception);
        _timerLogger ??= Services.GetService<ILoggerFactory>()?
            .CreateLogger("Zlink.Framework.SpotTimer");
        _timerLogger?.LogError(
            exception,
            "Spot timer handler failed. source={SourceName} spot={SpotId} entry={IsEntrySpot} timer={TimerName} handler={HandlerType} delivery={DeliveryIndex} scheduled={ScheduledIndex} stopped={Stopped}",
            sourceName,
            spotId,
            isEntrySpot,
            timerName,
            handlerType.FullName ?? handlerType.Name,
            tick.DeliveryIndex,
            tick.ScheduledIndex,
            stopped);
    }

    internal IServiceProvider Services { get; }

    internal ZLinkDrainAdmissionGate DrainAdmission => _drainAdmission;

    internal ZLinkRelocationPermitPool RelocationPermits =>
        _relocationPermits;

    internal ZLinkRelocationInterruptionObserver
        RelocationInterruption => _relocationInterruption;

    internal async ValueTask<bool> DrainStreamSessionsAsync(CancellationToken cancellationToken)
    {
        var state = _state;
        if (state is null) return true;
        var drained = true;
        foreach (var streamNode in state.StreamNodes.Values)
            drained &= await streamNode.DrainSessionsAsync(cancellationToken).ConfigureAwait(false);
        return drained;
    }

    internal async ValueTask<ZLinkSpotDrainResult> TryDrainSpotsAsync(
        bool relocate,
        bool hostShutdown,
        CancellationToken cancellationToken)
        => await TryDrainSpotsAsync(
                relocate,
                hostShutdown,
                DateTimeOffset.UtcNow + Registration.DefaultRequestTimeout,
                cancellationToken)
            .ConfigureAwait(false);

    internal async ValueTask<ZLinkSpotDrainResult> TryDrainSpotsAsync(
        bool relocate,
        bool hostShutdown,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken)
        => await TryDrainSpotsAsync(
                relocate,
                hostShutdown,
                ZLinkSpotRelocationPhase.Aggregates,
                absoluteDeadline,
                cancellationToken)
            .ConfigureAwait(false);

    private async ValueTask<ZLinkSpotDrainResult> TryDrainSpotsAsync(
        bool relocate,
        bool hostShutdown,
        ZLinkSpotRelocationPhase phase,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken)
    {
        var state = _state;
        if (state is null)
            return new ZLinkSpotDrainResult(
                true,
                0,
                null,
                ZLinkRelocationCommitKnowledge.NotCommitted,
                true);
        var drained = true;
        ulong committedUnitCount = 0;
        ZLinkFrameworkRelocationReason? terminalReason = null;
        var commitKnowledge = ZLinkRelocationCommitKnowledge.NotCommitted;
        var sourceTerminalized = true;
        foreach (var spotNode in state.SpotNodes.Values)
        {
            var result = await spotNode.TryDrainSpotsAsync(
                    relocate,
                    hostShutdown,
                    _relocationTargetSelection,
                    phase,
                    absoluteDeadline,
                    cancellationToken)
                .ConfigureAwait(false);
            drained &= result.Completed;
            terminalReason ??= result.TerminalReason;
            committedUnitCount = checked(
                committedUnitCount + result.CommittedUnitCount);
            commitKnowledge = CombineCommitKnowledge(
                commitKnowledge,
                result.CommitKnowledge,
                committedUnitCount);
            sourceTerminalized &= result.SourceTerminalized;
        }
        return new ZLinkSpotDrainResult(
            drained,
            committedUnitCount,
            terminalReason,
            commitKnowledge,
            sourceTerminalized);
    }

    private static ZLinkRelocationCommitKnowledge CombineCommitKnowledge(
        ZLinkRelocationCommitKnowledge left,
        ZLinkRelocationCommitKnowledge right,
        ulong committedUnitCount)
    {
        if (left == ZLinkRelocationCommitKnowledge.Unknown
            || right == ZLinkRelocationCommitKnowledge.Unknown)
            return ZLinkRelocationCommitKnowledge.Unknown;
        return left == ZLinkRelocationCommitKnowledge.Committed
               || right == ZLinkRelocationCommitKnowledge.Committed
               || committedUnitCount != 0
            ? ZLinkRelocationCommitKnowledge.Committed
            : ZLinkRelocationCommitKnowledge.NotCommitted;
    }

    internal async ValueTask<ZLinkRelocationWorkloadDrainResult>
        DrainRelocationWorkloadsAsync(
            ZLinkRelocationWorkloadDrainControl control)
        => await new ZLinkRelocationWorkloadCoordinator(
                (phase, token) => TryDrainSpotsAsync(
                    relocate: true,
                    hostShutdown: false,
                    phase,
                    control.AbsoluteDeadline,
                    token),
                DrainActorsAsync)
            .DrainAsync(control)
            .ConfigureAwait(false);

    internal async ValueTask<bool> QuiesceServingChannelsForDrainAsync(
        ZLinkLocationAutoConnectHost? autoConnect,
        CancellationToken cancellationToken)
    {
        var state = _state;
        if (state is null) return true;

        // Spec 28 §11 seals application admission first (step 1) and publishes the
        // Draining descriptor second (step 2), so a caller can still select this
        // node between the two. Zeroing the serving weight here narrows that
        // window and removes the node from peer load balancing before owner
        // cleanup can terminate the pipes, but it cannot close the window:
        // propagation is asynchronous. What closes it is caller-side
        // reselection on the `ShuttingDown` answer (spec 06 §13.1), in
        // ZLinkChannelRequestCall.
        var published = true;
        foreach (var (name, spotNode) in state.SpotNodes)
        {
            if (!Registration.SpotNodes.TryGetValue(name, out var registration))
                continue;

            foreach (var membership in registration.ChannelMemberships)
            {
                // Client membership does not own a serving weight. Only a
                // RouteMesh Channel Server is removed from new selection
                // before drain seals application admission.
                if (membership.IsServer)
                    spotNode.Node.SetChannelWeight(
                        membership.ChannelName,
                        0);
            }

            //  Spec 28 §11 step 2. Weight 0은 이 node를 선택에서 빼지만 peer가
            //  "왜"를 알지 못한다. Draining descriptor를 함께 게시해야 peer의
            //  status·monitoring이 draining을 관측하고 placement에서도 빠진다.
            spotNode.Node.PublishDraining();

            var meshName = registration.SpotMeshChannelName ?? registration.SpotNodeName;
            published &= await PublishWeightAsync(
                autoConnect,
                ZLinkLocationAutoConnectType.SpotMesh,
                meshName,
                ZLinkLocationRole.Spot,
                cancellationToken).ConfigureAwait(false);
        }

        return published;
    }

    private static ValueTask<bool> PublishWeightAsync(
        ZLinkLocationAutoConnectHost? autoConnect,
        ZLinkLocationAutoConnectType type,
        string meshName,
        ZLinkLocationRole role,
        CancellationToken cancellationToken) =>
        autoConnect is null
            ? ValueTask.FromResult(true)
            : autoConnect.SetLocalWeightAsync(type, meshName, role, 0, cancellationToken);

    //  Relocate도 이 node를 새 selection에서 빼야 한다. Shutdown 경로는
    //  QuiesceServingChannelsForDrainAsync가 weight 0과 함께 게시하지만,
    //  Relocate는 그 경로를 타지 않으므로 별도 진입점이 필요하다.
    internal void PublishDrainingToPeers()
    {
        var state = _state;
        if (state is null) return;
        foreach (var (_, spotNode) in state.SpotNodes)
            spotNode.Node.PublishDraining();
    }

    internal void SealApplicationAdmissionsForDrain()
    {
        //  Shutdown claims the admission owner before taking the runtime lock.
        //  Relocation fence and shutdown therefore use the same drain-gate ->
        //  operation-gate order without allowing rollback to reopen shutdown.
        _drainAdmission.ClaimShutdown();
        lock (_operationGate)
        {
            _drainAdmission.Seal();
            _acceptingOperations = false;
            _admissionOwner = ZLinkDrainOwner.Shutdown;
        }
    }

    internal void ReopenRetireAdmissionsAfterRollback() =>
        _ = TryReopenRetireAdmissionsAfterRollback();

    internal bool TryReopenRetireAdmissionsAfterRollback()
    {
        var expectedGeneration = Volatile.Read(ref _relocationFenceGeneration);
        return expectedGeneration == 0
            ? false
            : TryReopenRetireAdmissionsAfterRollback(
                new ZLinkRelocationAdmissionFence(expectedGeneration));
    }

    internal bool TryReopenRetireAdmissionsAfterRollback(
        ZLinkRelocationAdmissionFence expectedFence)
    {
        return _drainAdmission.TryReopenRelocationFence(() =>
        {
            lock (_operationGate)
            {
                if (_admissionOwner != ZLinkDrainOwner.Relocation
                    || _relocationFenceGeneration != expectedFence.Generation)
                    return false;
                if (!_actorHandoffAdmissions.SnapshotDrain().IsSafe)
                    return false;
                _admissionOwner = ZLinkDrainOwner.None;
                _relocationFenceGeneration = 0;
                _acceptingOperations = true;
                return true;
            }
        });
    }

    internal Task WaitForAcceptedOperationsForDrainAsync()
    {
        lock (_operationGate)
            return _activeOperations == 0
                ? Task.CompletedTask
                : (_operationsAtZero ??= new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously)).Task;
    }

    internal ZLinkRuntimeOperationAdmissionSnapshot
        SnapshotOperationAdmissions()
    {
        lock (_operationGate)
            return new(_operationEpoch, _activeOperations);
    }

    internal ZLinkRelocationAdmissionFence?
        CaptureRelocationAdmissionFence()
    {
        lock (_operationGate)
            return _admissionOwner == ZLinkDrainOwner.Relocation
                   && _relocationFenceGeneration != 0
                ? new ZLinkRelocationAdmissionFence(_relocationFenceGeneration)
                : null;
    }

    internal ZLinkRelocationRollbackLease?
        TryAcquireRelocationRollbackLease(
            ZLinkRelocationAdmissionFence expectedFence)
    {
        _drainAdmission.TryAcquireRelocationRollbackLease(
            () =>
            {
                lock (_operationGate)
                {
                    var acquired = _admissionOwner == ZLinkDrainOwner.Relocation
                                   && _relocationFenceGeneration
                                      == expectedFence.Generation
                                   && _activeOperations == 0
                                   && _actorHandoffAdmissions.SnapshotDrain().IsSafe;
                    if (acquired)
                        _admissionOwner = ZLinkDrainOwner.RelocationRollback;
                    return acquired;
                }
            },
            out var lease);
        return lease;
    }

    internal bool TryReopenRetireAdmissionsAfterRollback(
        ZLinkRelocationAdmissionFence expectedFence,
        ZLinkRelocationRollbackLease lease)
    {
        return _drainAdmission.TryCompleteRelocationRollbackLease(
            lease,
            () =>
            {
                lock (_operationGate)
                {
                    if (_admissionOwner != ZLinkDrainOwner.RelocationRollback
                        || _relocationFenceGeneration
                           != expectedFence.Generation
                        || !_actorHandoffAdmissions.SnapshotDrain().IsSafe)
                        return false;
                    _admissionOwner = ZLinkDrainOwner.None;
                    _relocationFenceGeneration = 0;
                    _acceptingOperations = true;
                    return true;
                }
            });
    }

    internal ValueTask<ZLinkRelocationAdmissionFence?>
        TryBeginRelocationAdmissionFenceAsync(
            ZLinkRuntimeOperationAdmissionSnapshot operationBaseline,
            ZLinkActorAdmissionSnapshot actorBaseline,
            ZLinkActorHandoffDrainSnapshot handoffBaseline,
            CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ZLinkRelocationAdmissionFence? fence = null;
        var committed = _drainAdmission.TryBeginRelocationFence(actorSnapshot =>
        {
            lock (_operationGate)
            {
                var currentOperation = new ZLinkRuntimeOperationAdmissionSnapshot(
                    _operationEpoch,
                    _activeOperations);
                var currentHandoff = _actorHandoffAdmissions.SnapshotDrain();
                if (_admissionOwner != ZLinkDrainOwner.None
                    || !_acceptingOperations
                    || currentOperation != operationBaseline
                    || actorSnapshot != actorBaseline
                    || currentHandoff != handoffBaseline
                    || !currentHandoff.IsSafe)
                    return false;

                _drainAdmission.Seal();
                _acceptingOperations = false;
                _admissionOwner = ZLinkDrainOwner.Relocation;
                var generation = checked(++_nextRelocationFenceGeneration);
                _relocationFenceGeneration = generation;
                fence = new ZLinkRelocationAdmissionFence(generation);
                return true;
            }
        });
        if (!committed)
            return ValueTask.FromResult<ZLinkRelocationAdmissionFence?>(null);

        //  Relocation admission is now sealed, so no new host operation can
        //  enter. Existing application turns must be drained by their owning
        //  Spot, where the accepted-turn boundary and any relocation-ready
        //  callback are known. Waiting for the host-wide count here would
        //  block the marker publication that those turns may be waiting for.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_admission_fence_committed active_operations="
            + $"{operationBaseline.ActiveCount} "
            + $"active_actor_admissions={actorBaseline.ActiveCount}");
        return ValueTask.FromResult(fence);
    }

    internal async Task WaitForAcceptedActorHandoffsAsync(CancellationToken cancellationToken)
    {
        await _drainAdmission.WaitForAcceptedActorAdmissionsAsync(cancellationToken)
            .ConfigureAwait(false);
        await _actorHandoffAdmissions.WaitUntilDrainSafeAsync(cancellationToken)
            .ConfigureAwait(false);
    }

    internal ZLinkDrainRemainderCounts GetDrainRemainderCounts()
    {
        var actors = _actorSessionManager.SnapshotStates()
            .Count(static actor => actor.Actor is not null);
        var state = _state;
        var spots = state?.SpotNodes.Values.Sum(static node => node.Spots.Count) ?? 0;
        var sessions = state?.StreamNodes.Values.Sum(static node => node.SessionCount) ?? 0;
        int requests;
        lock (_operationGate) requests = _activeRequests;
        return new ZLinkDrainRemainderCounts(actors, spots, requests, sessions);
    }

    internal object ExecutionOwner
    {
        get
        {
            var current = Volatile.Read(ref _executionScope);
            if (current is not null) return current;
            var created = new ZLinkRuntimeExecutionScope();
            return Interlocked.CompareExchange(ref _executionScope, created, null) ?? created;
        }
    }

    internal ZLinkRuntimeOperationLease EnterOperation(bool countAsRequest = false)
    {
        if (AmbientOperation.Value is { IsActive: true } current
            && ReferenceEquals(current.Runtime, this))
        {
            EnsureAmbientOperationCurrent(current);
            if (!countAsRequest) return new ZLinkRuntimeOperationLease();
            lock (_operationGate) _activeRequests++;
            return new ZLinkRuntimeOperationLease(this, countsRequest: true);
        }

        lock (_operationGate)
            return EnterOperationUnderLock(countAsRequest);
    }

    /// <summary>
    /// Entry for an operational read. Spec 28 §13 has individual blockers and
    /// relocation state checked through bounded diagnostic queries, so a
    /// location lookup still has to answer once the host stopped accepting
    /// work; the sibling reads on the operational surface take no gate at all.
    /// The lease still counts, so shutdown waits for a read in flight, and a
    /// runtime past Running still refuses.
    /// </summary>
    internal ZLinkRuntimeOperationLease EnterOperationalRead()
    {
        if (AmbientOperation.Value is { IsActive: true } current
            && ReferenceEquals(current.Runtime, this))
        {
            EnsureAmbientOperationCurrent(current);
            return new ZLinkRuntimeOperationLease();
        }

        lock (_operationGate)
        {
            if (Volatile.Read(ref _lifecyclePhase) != (int)ZLinkRuntimeLifecyclePhase.Running
                || _state is not { } state)
                throw new InvalidOperationException(
                    "ZLink framework runtime is not accepting operations.");
            _activeOperations++;
            _operationEpoch++;
            var previous = AmbientOperation.Value;
            var ownership = new ZLinkRuntimeOperationOwnership(this, state);
            AmbientOperation.Value = ownership;
            return new ZLinkRuntimeOperationLease(this, ownership, previous, false);
        }
    }

    internal ZLinkRuntimeOperationLease RetainOperationForBackgroundWork()
    {
        if (AmbientOperation.Value is not { IsActive: true } current
            || !ReferenceEquals(current.Runtime, this))
            throw new InvalidOperationException(
                "Background work can retain only the current runtime operation.");
        EnsureAmbientOperationCurrent(current);

        lock (_operationGate)
        {
            _activeOperations++;
            _operationEpoch++;
            return new ZLinkRuntimeOperationLease(
                this,
                countsOperation: true,
                countsRequest: false);
        }
    }

    /// <summary>
    /// <paramref name="ownsObjectWork"/>: false for dispatch that only invokes
    /// a registered handler - channel, node route and fanout records. Spec 21
    /// §4 blocks an expired owner from descriptor publishing, Actor, Spot and
    /// Instance message and timer starts, factory and restore commits, and
    /// relocation state, because "만료된 owner 자격으로 새 Store 변경을 만들지
    /// 않는다". Handler invocation makes no Store change, so a Location Store
    /// outage must not turn it away. The drain seal still applies to every
    /// caller.
    /// </summary>
    internal bool TryEnterInboundOperation(
        bool countAsRequest,
        out ZLinkRuntimeOperationLease lease,
        bool ownsObjectWork = true)
    {
        if (AmbientOperation.Value is { IsActive: true } current
            && ReferenceEquals(current.Runtime, this))
        {
            if (!IsAmbientOperationCurrent(current))
            {
                lease = default!;
                return false;
            }
            if (!countAsRequest)
            {
                lease = new ZLinkRuntimeOperationLease();
                return true;
            }
            lock (_operationGate) _activeRequests++;
            lease = new ZLinkRuntimeOperationLease(this, countsRequest: true);
            return true;
        }

        lock (_operationGate)
        {
            if (_drainAdmission.IsSealed
                || (ownsObjectWork
                    && _locationRuntime is not null
                    && !_locationRuntime.IsOwnerAdmissionOpen))
            {
                lease = new ZLinkRuntimeOperationLease();
                return false;
            }
            // Before native startup no transport can deliver a record. A
            // neutral lease keeps dispatcher construction independent from
            // runtime startup while the drain seal remains authoritative.
            if (Volatile.Read(ref _lifecyclePhase) != (int)ZLinkRuntimeLifecyclePhase.Running)
            {
                lease = new ZLinkRuntimeOperationLease();
                return true;
            }
            lease = EnterOperationUnderLock(countAsRequest);
            return true;
        }
    }

    internal async ValueTask ExecuteOperationAsync(Func<ValueTask> operation)
    {
        using var lease = EnterOperation();
        await operation().ConfigureAwait(false);
    }

    internal async ValueTask<T> ExecuteOperationAsync<T>(Func<ValueTask<T>> operation)
    {
        using var lease = EnterOperation();
        return await operation().ConfigureAwait(false);
    }

    internal T ExecuteOperation<T>(Func<T> operation)
    {
        using var lease = EnterOperation();
        return operation();
    }

    internal ZLinkWorkerPool WorkerPool
    {
        get
        {
            if (!IsStarted)
                throw new InvalidOperationException("ZLink framework runtime is not running.");
            lock (_workerPoolGate)
            {
                if (!IsStarted || _state is null)
                    throw new InvalidOperationException("ZLink framework runtime is not running.");
                return _workerPool ??= Registration.WorkerOptions.CreatePool();
            }
        }
    }

    // Logical Multicast admission is isolated from handler and activation work.
    // A remote target can remain unavailable after a node failure; that state
    // must not consume the worker capacity used by the rest of the runtime.
    internal ZLinkWorkerPool LogicalMulticastWorkerPool
    {
        get
        {
            if (!IsStarted)
                throw new InvalidOperationException("ZLink framework runtime is not running.");
            lock (_workerPoolGate)
            {
                if (!IsStarted || _state is null)
                    throw new InvalidOperationException("ZLink framework runtime is not running.");
                return _logicalMulticastWorkerPool ??= Registration.WorkerOptions.CreatePool();
            }
        }
    }

    internal IZLinkRouteClient RouteClient => Services.GetRequiredService<IZLinkRouteClient>();

    internal ZLinkSpotNodeRuntime GetSpotNodeRuntime(RoutingId nodeRid)
    {
        var state = _state
                    ?? throw new InvalidOperationException(
                        "The framework runtime is not started.");
        return state.SpotNodes.Values.SingleOrDefault(
                   node => node.Node.RoutingId == nodeRid)
               ?? throw new ZLinkFrameworkException(
                   ZLinkFrameworkErrorKind.NotFound,
                   $"MeshNode '{nodeRid}' is not hosted by this runtime.");
    }

    internal ZLinkSpotNodeRuntime? TryGetSpotNodeRuntime(RoutingId nodeRid) =>
        _state?.SpotNodes.Values.SingleOrDefault(
            node => node.Node.RoutingId == nodeRid);

    public bool IsStarted
        => Volatile.Read(ref _lifecyclePhase) == (int)ZLinkRuntimeLifecyclePhase.Running;

    internal CancellationToken ShutdownToken
        => _state?.StopTokenSource.Token ?? new CancellationToken(canceled: true);

    internal ZLinkCompletionAdmissionOwner CompletionAdmission =>
        GetOrStartState().CompletionAdmission;

    internal ZLinkInboundDispatchStatus SnapshotInboundDispatch()
    {
        var snapshot = Volatile.Read(ref _state)?.InboundDispatchBudget.Snapshot()
                       ?? new ZLinkInboundDispatchBudgetSnapshot(
                           Registration.InboundDispatchOptions
                               .EffectiveApplicationHwmBytes,
                           0,
                           0,
                           0,
                           false);
        var completion = Volatile.Read(ref _state)?
            .CompletionAdmission.Snapshot();
        return new ZLinkInboundDispatchStatus(
            snapshot.ApplicationHwmBytes,
            snapshot.PendingPayloadBytes,
            snapshot.QueuedPayloadBytes,
            snapshot.ActivePayloadBytes,
            snapshot.ApplicationReceivePaused,
            PendingCompletionSends: checked((ulong)(
                completion?.PendingCompletionSends ?? 0)),
            CompletionSendLimit: checked((ulong)(
                completion?.CompletionSendLimit ?? 0)));
    }

    internal void RunDetached(
        string name,
        Func<CancellationToken, ValueTask> callback)
    {
        _ = TryRunDetached(name, callback);
    }

    internal bool TryRunDetached(
        string name,
        Func<CancellationToken, ValueTask> callback)
    {
        var state = AmbientOperation.Value is { IsActive: true } operation
                    && ReferenceEquals(operation.Runtime, this)
            ? operation.State
            : Volatile.Read(ref _executionScope) is { } executionScope
              && ZLinkRuntimeTaskRunner.IsCurrentExecutionFor(executionScope)
                ? _state
            : IsStarted
                ? _state
                : null;
        return state is not null && state.TaskRunner.TryRunDetached(name, callback);
    }

    internal ValueTask<ZLinkFrameworkComponentState> GetStartedStateForRoutingAsync(
        CancellationToken cancellationToken)
    {
        return GetStartedStateAsync(cancellationToken);
    }

    internal bool IsAcceptingApplicationWork
    {
        get
        {
            lock (_operationGate)
                return _acceptingOperations;
        }
    }

    internal RoutingId PrepareLocationNodeRoutingId()
    {
        var registration = Registration.SpotNodes.Values.FirstOrDefault();
        return registration is null
            ? RoutingId.From(Guid.NewGuid().ToString("n"))
            : ZLinkSpotNodeInitializer.PrepareNodeRoutingId(registration);
    }

    public async ValueTask StartAsync(CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken);
        try
        {
            if (Volatile.Read(ref _lifecyclePhase) == (int)ZLinkRuntimeLifecyclePhase.Running) return;

            Volatile.Write(ref _lifecyclePhase, (int)ZLinkRuntimeLifecyclePhase.Starting);
            try
            {
                _state = await _stateFactory.CreateAsync().ConfigureAwait(false);
                // A published relocation is not replayed during process startup.
                // Recovery remains an explicit same-process operation; a new
                // process must rediscover the current owner through the normal
                // location lifecycle instead of taking over a prior journal.
                lock (_operationGate) _acceptingOperations = true;
                Volatile.Write(ref _lifecyclePhase, (int)ZLinkRuntimeLifecyclePhase.Running);
                _locationLifecycle?.ResumeBackgroundWork();
            }
            catch (Exception startFailure)
            {
                Volatile.Write(ref _lifecyclePhase, (int)ZLinkRuntimeLifecyclePhase.Stopping);
                await StopAcceptingOperationsAsync().ConfigureAwait(false);
                var failures = await CleanupRuntimeGenerationAsync(_state).ConfigureAwait(false);
                _state = null;
                Interlocked.Exchange(ref _executionScope, null);
                Volatile.Write(ref _lifecyclePhase, (int)ZLinkRuntimeLifecyclePhase.Stopped);
                ThrowCleanupFailures(failures, startFailure);
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    public async ValueTask StopAsync(CancellationToken cancellationToken)
    {
        ThrowIfStopRequestedFromOwnedWork();
        await _gate.WaitAsync(cancellationToken);
        try
        {
            ThrowIfStopRequestedFromOwnedWork();
            if ((ZLinkRuntimeLifecyclePhase)Volatile.Read(ref _lifecyclePhase)
                == ZLinkRuntimeLifecyclePhase.Stopped)
                return;
            var stateToDispose = _state;
            Volatile.Write(ref _lifecyclePhase, (int)ZLinkRuntimeLifecyclePhase.Stopping);
            try
            {
                var operationsDrained = StopAcceptingOperationsAsync();
                stateToDispose?.CancelActiveSpotOperations();
                await operationsDrained.ConfigureAwait(false);
                var failures = await CleanupRuntimeGenerationAsync(stateToDispose).ConfigureAwait(false);
                ThrowCleanupFailures(failures);
            }
            finally
            {
                _state = null;
                Interlocked.Exchange(ref _executionScope, null);
                Volatile.Write(ref _lifecyclePhase, (int)ZLinkRuntimeLifecyclePhase.Stopped);
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    /// <summary>
    /// Stops a runtime after the drain deadline has already expired. Unlike
    /// the normal stop path, this does not wait for active operations before
    /// disposing their owners: stream-session and worker disposal cancel the
    /// corresponding execution tokens and bound their cleanup.
    /// </summary>
    internal async ValueTask ForceStopAsync(CancellationToken cancellationToken)
    {
        ThrowIfStopRequestedFromOwnedWork();
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            ThrowIfStopRequestedFromOwnedWork();
            if ((ZLinkRuntimeLifecyclePhase)Volatile.Read(ref _lifecyclePhase)
                == ZLinkRuntimeLifecyclePhase.Stopped)
                return;

            var stateToDispose = _state;
            Volatile.Write(ref _lifecyclePhase, (int)ZLinkRuntimeLifecyclePhase.Stopping);
            try
            {
                lock (_operationGate) _acceptingOperations = false;
                stateToDispose?.FenceOperations();
            stateToDispose?.CancelActiveSpotOperations();
            stateToDispose?.ForceStopStreamSessions();
            // ForceStop must remain forceful even when the caller does not
            // supply an external cancellation deadline. The linked token
            // selects the component state's non-graceful disposal path.
            using var forceStop =
                CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken);
            var failures = await CleanupRuntimeGenerationAsync(
                        stateToDispose,
                        forceStop.Token)
                    .ConfigureAwait(false);
                if (cancellationToken.IsCancellationRequested
                    && failures.Count > 0)
                    throw new OperationCanceledException(
                        "Framework force-stop exceeded its cleanup deadline.",
                        failures.Count == 1
                            ? failures[0]
                            : new AggregateException(failures),
                        cancellationToken);
                ThrowCleanupFailures(failures);
            }
            finally
            {
                _state = null;
                Interlocked.Exchange(ref _executionScope, null);
                Volatile.Write(ref _lifecyclePhase, (int)ZLinkRuntimeLifecyclePhase.Stopped);
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    private void ThrowIfStopRequestedFromOwnedWork()
    {
        if (Volatile.Read(ref _executionScope) is { } executionScope
            && ZLinkRuntimeTaskRunner.IsCurrentExecutionFor(executionScope))
            throw new InvalidOperationException(
                "The framework runtime cannot stop from one of its own managed tasks. Request shutdown from an external lifecycle owner.");
        if (AmbientOperation.Value is { IsActive: true } operation
            && ReferenceEquals(operation.Runtime, this))
            throw new InvalidOperationException(
                "The framework runtime cannot stop from one of its active operations. Request shutdown from an external lifecycle owner.");
    }

    private async ValueTask<List<Exception>> CleanupRuntimeGenerationAsync(
        ZLinkFrameworkComponentState? state,
        CancellationToken forceStopToken = default)
    {
        var failures = new List<Exception>();
        ZLinkWorkerPool? workerPool;
        ZLinkWorkerPool? logicalMulticastWorkerPool;
        var logicalMulticastPoolDisposed = false;
        lock (_workerPoolGate)
        {
            workerPool = _workerPool;
            _workerPool = null;
            logicalMulticastWorkerPool = _logicalMulticastWorkerPool;
            _logicalMulticastWorkerPool = null;
        }

        if (workerPool is not null) Capture(workerPool.RequestStop);
        if (logicalMulticastWorkerPool is not null)
            Capture(logicalMulticastWorkerPool.RequestStop);
        // ForceStop does not wait for the runtime operation drain. Stop the
        // logical multicast lane before disposing MeshNode/socket owners so a
        // committed worker cannot begin target processing against torn-down
        // state.
        if (forceStopToken.CanBeCanceled && logicalMulticastWorkerPool is not null)
        {
            await CaptureAsync(logicalMulticastWorkerPool.DisposeAsync).ConfigureAwait(false);
            logicalMulticastPoolDisposed = true;
        }
        if (_locationLifecycle is not null)
            await CaptureAsync(_locationLifecycle.PauseBackgroundWorkAsync).ConfigureAwait(false);
        var generationCleanupReporter =
            (state?.ErrorSink ?? Volatile.Read(ref _generationErrorSink))
            ?.CaptureGenerationReporter();
        await CaptureAsync(
                () => ResetActorRuntimeGenerationAsync(
                    forceStopToken,
                    generationCleanupReporter))
            .ConfigureAwait(false);
        if (state is not null)
            await CaptureAsync(forceStopToken.CanBeCanceled
                    ? () => state.ForceStopAsync(forceStopToken)
                    : state.DisposeAsync)
                .ConfigureAwait(false);
        Capture(_standaloneActorRelocationRuntime.ReleaseRetainedPermits);
        if (state is not null) DetachErrorSink(state.ErrorSink);
        if (_locationLifecycle is not null)
            await CaptureAsync(_locationLifecycle.PauseBackgroundWorkAsync).ConfigureAwait(false);

        if (workerPool is not null) await CaptureAsync(workerPool.DisposeAsync).ConfigureAwait(false);
        if (logicalMulticastWorkerPool is not null && !logicalMulticastPoolDisposed)
            await CaptureAsync(logicalMulticastWorkerPool.DisposeAsync).ConfigureAwait(false);
        return failures;

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }

        void Capture(Action cleanup)
        {
            try
            {
                cleanup();
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }
    }

    private static void ThrowCleanupFailures(
        IReadOnlyList<Exception> cleanupFailures,
        Exception? primaryFailure = null)
    {
        if (primaryFailure is null && cleanupFailures.Count == 0) return;
        if (primaryFailure is not null && cleanupFailures.Count == 0)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(primaryFailure).Throw();
        if (primaryFailure is null && cleanupFailures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(cleanupFailures[0]).Throw();

        throw new AggregateException(
            primaryFailure is null
                ? cleanupFailures
                : new[] { primaryFailure }.Concat(cleanupFailures));
    }

    private async ValueTask<ZLinkFrameworkComponentState> GetStartedStateAsync(
        CancellationToken cancellationToken)
    {
        var phase = (ZLinkRuntimeLifecyclePhase)Volatile.Read(ref _lifecyclePhase);
        if (phase is ZLinkRuntimeLifecyclePhase.Stopping or ZLinkRuntimeLifecyclePhase.Starting)
            throw new InvalidOperationException($"ZLink framework runtime is {phase.ToString().ToLowerInvariant()}.");
        if (phase == ZLinkRuntimeLifecyclePhase.Stopped) await StartAsync(cancellationToken);

        return IsStarted && _state is { } state
            ? state
            : throw new InvalidOperationException("ZLink framework runtime is not started.");
    }

    private ZLinkFrameworkComponentState GetOrStartState()
    {
        if (!IsStarted || _state is null)
            throw new InvalidOperationException(
                "ZLink framework runtime is not started. Call StartAsync before using synchronous runtime APIs.");

        return _state;
    }

    private Task StopAcceptingOperationsAsync()
    {
        lock (_operationGate)
        {
            _acceptingOperations = false;
            if (_activeOperations == 0) return Task.CompletedTask;
            return (_operationsDrained ??= new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously)).Task;
        }
    }

    private ZLinkRuntimeOperationLease EnterOperationUnderLock(bool countAsRequest)
    {
        //  네 조건을 한 문장으로 뭉치면 호출자는 admission seal인지, 아직 기동
        //  중인지, owner lease가 닫힌 것인지 구분할 수 없다. 조사할 때마다 여기에
        //  계측을 다시 심게 되므로 어느 조건인지 메시지에 남긴다.
        var refusal =
            !_acceptingOperations ? "application admission is sealed"
            : Volatile.Read(ref _lifecyclePhase) != (int)ZLinkRuntimeLifecyclePhase.Running
                ? $"lifecycle phase is {(ZLinkRuntimeLifecyclePhase)Volatile.Read(ref _lifecyclePhase)}"
            : _state is null ? "runtime state is not created"
            : _locationRuntime is not null && !_locationRuntime.IsOwnerAdmissionOpen
                ? "owner admission is closed"
            : null;
        if (refusal is not null)
            throw new InvalidOperationException(
                $"ZLink framework runtime is not accepting operations ({refusal}).");
        if (_state is not { } state)
            throw new InvalidOperationException(
                "ZLink framework runtime is not accepting operations (runtime state is not created).");
        _activeOperations++;
        _operationEpoch++;
        if (countAsRequest) _activeRequests++;
        var previous = AmbientOperation.Value;
        var ownership = new ZLinkRuntimeOperationOwnership(this, state);
        AmbientOperation.Value = ownership;
        return new ZLinkRuntimeOperationLease(this, ownership, previous, countAsRequest);
    }

    private bool IsAmbientOperationCurrent(
        ZLinkRuntimeOperationOwnership ownership) =>
        !ownership.State.IsOperationFenced
        && ReferenceEquals(ownership.State, Volatile.Read(ref _state));

    private void EnsureAmbientOperationCurrent(
        ZLinkRuntimeOperationOwnership ownership)
    {
        if (!IsAmbientOperationCurrent(ownership))
            throw new InvalidOperationException(
                "The framework runtime operation belongs to a stopped generation.");
    }

    private void ExitOperation(bool countsOperation, bool countsRequest)
    {
        TaskCompletionSource? drained = null;
        TaskCompletionSource? atZero = null;
        lock (_operationGate)
        {
            if (countsRequest && --_activeRequests < 0)
                throw new InvalidOperationException("Runtime request lease count became negative.");
            if (countsOperation && --_activeOperations < 0)
                throw new InvalidOperationException("Runtime operation lease count became negative.");
            if (countsOperation)
                _operationEpoch++;
            if (_activeOperations == 0)
            {
                drained = _operationsDrained;
                _operationsDrained = null;
                atZero = _operationsAtZero;
                _operationsAtZero = null;
            }
        }
        drained?.TrySetResult();
        atZero?.TrySetResult();
    }

    internal sealed class ZLinkRuntimeOperationLease : IDisposable
    {
        private readonly ZLinkFrameworkRuntime? _runtime;
        private readonly ZLinkRuntimeOperationOwnership? _ownership;
        private readonly ZLinkRuntimeOperationOwnership? _previous;
        private readonly bool _countsOperation;
        private readonly bool _countsRequest;
        private int _disposed;

        internal ZLinkRuntimeOperationLease()
        {
        }

        internal ZLinkRuntimeOperationLease(
            ZLinkFrameworkRuntime runtime,
            bool countsRequest)
            : this(runtime, countsOperation: false, countsRequest)
        {
        }

        internal ZLinkRuntimeOperationLease(
            ZLinkFrameworkRuntime runtime,
            bool countsOperation,
            bool countsRequest)
        {
            _runtime = runtime;
            _countsOperation = countsOperation;
            _countsRequest = countsRequest;
        }

        internal ZLinkRuntimeOperationLease(
            ZLinkFrameworkRuntime runtime,
            ZLinkRuntimeOperationOwnership ownership,
            ZLinkRuntimeOperationOwnership? previous,
            bool countsRequest)
        {
            _runtime = runtime;
            _ownership = ownership;
            _previous = previous;
            _countsOperation = true;
            _countsRequest = countsRequest;
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
            {
                _ownership?.Deactivate();
                if (_ownership is not null && ReferenceEquals(AmbientOperation.Value, _ownership))
                    AmbientOperation.Value = _previous;
                _runtime?.ExitOperation(_countsOperation, _countsRequest);
            }
        }
    }

    internal sealed class ZLinkRuntimeOperationOwnership(
        ZLinkFrameworkRuntime runtime,
        ZLinkFrameworkComponentState state)
    {
        private int _active = 1;

        public ZLinkFrameworkRuntime Runtime { get; } = runtime;

        public ZLinkFrameworkComponentState State { get; } = state;

        public bool IsActive => Volatile.Read(ref _active) != 0;

        public void Deactivate() => Interlocked.Exchange(ref _active, 0);
    }
}

internal enum ZLinkRuntimeLifecyclePhase
{
    Stopped,
    Running,
    Stopping,
    Starting
}
