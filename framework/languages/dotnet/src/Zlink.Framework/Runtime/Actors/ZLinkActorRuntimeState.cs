using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Identifiers;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorRuntimeState(
    ZLinkActorId actorId,
    TimeProvider? timeProvider = null,
    Action<string>? handoffDiagnostic = null,
    ZLinkBoundedIngressAdmission? sourceIngressAdmission = null,
    TimeSpan? sessionBindingTombstoneRetention = null,
    int maxSessionBindingTombstones = 1_024,
    IServiceProvider? services = null
)
{
    private readonly ZLinkActorId _actorId = actorId;
    private static readonly IServiceProvider EmptyServices = new EmptyServiceProvider();
    private static readonly TimeSpan DefaultSessionBindingTombstoneRetention = TimeSpan.FromMinutes(
        2
    );
    private static readonly AsyncLocal<DispatchOwnership?> AmbientDispatch = new();
    private readonly ZLinkActorSerialExecutor _dispatchMailbox = new();
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly ZLinkStateLane _lane = new();
    private readonly TimeProvider _timeProvider = timeProvider ?? TimeProvider.System;
    private readonly TimeSpan _sessionBindingTombstoneRetention =
        sessionBindingTombstoneRetention is { } configured && configured > TimeSpan.Zero
            ? configured
            : DefaultSessionBindingTombstoneRetention;
    private readonly int _maxSessionBindingTombstones =
        maxSessionBindingTombstones > 0 ? maxSessionBindingTombstones : 1_024;
    private readonly Dictionary<
        string,
        ZLinkActorSessionBindingTombstone
    > _sessionBindingTombstones = new(StringComparer.Ordinal);
    private Task<IZLinkActor>? _actorCreationTask;
    private int _actorMetricActive;
    private string? _actorMetricMeshName;
    private ZLinkActorBoundSession? _boundSession;
    private ZLinkPendingActorSessionRoute? _pendingSessionRoute;
    private ZLinkSourceSessionRelocation? _sourceSessionRelocation;
    private TaskCompletionSource<Exception?>? _teardownAttempt;
    private readonly IServiceProvider _services = services ?? EmptyServices;
    private ZLinkActorHandlerActivation? _handlerActivation;
    private bool _handlerActivationClosed;
    private Task? _terminalLifecycleCompletion;
    private int _contextInvalidated;

    internal ZLinkActorRuntimeState(
        string actorId,
        TimeProvider? timeProvider = null,
        Action<string>? handoffDiagnostic = null,
        ZLinkBoundedIngressAdmission? sourceIngressAdmission = null,
        TimeSpan? sessionBindingTombstoneRetention = null,
        int maxSessionBindingTombstones = 1_024,
        IServiceProvider? services = null
    )
        : this(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            timeProvider,
            handoffDiagnostic,
            sourceIngressAdmission,
            sessionBindingTombstoneRetention,
            maxSessionBindingTombstones,
            services
        ) { }

    public string ActorId => _actorId.Value;

    internal int PendingLifecycleCount => _dispatchMailbox.PendingLifecycleCount;

    internal ZLinkActorId RuntimeActorId => _actorId;

    public ZLinkActorHandoffState Handoff { get; } =
        new(
            actorId.Value,
            timeProvider ?? TimeProvider.System,
            handoffDiagnostic,
            sourceIngressAdmission
        );

    public string? ActorType { get; private set; }

    public string? SessionId { get; private set; }

    public IZLinkStream? Stream { get; private set; }

    public ZLinkBackendActorRef? NativeActorRef { get; private set; }

    public ZLinkBackendActorRef? RetiredLocalActorRef { get; private set; }

    public ZLinkSpotActivation? Activation { get; private set; }

    public ZLinkActorContext? Context { get; private set; }

    public IZLinkActor? Actor { get; private set; }

    /// <summary>MeshNode that activated this Actor in the current process.</summary>
    internal string? MeshName => Activation?.MeshName ?? Context?.MeshName;

    private T RunState<T>(Func<T> operation) => AwaitStateLane(_lane.RunAsync(operation));

    private void RunState(Action operation) => AwaitStateLane(_lane.RunAsync(operation));

    internal ZLinkScopedHandlerInstanceOwner HandlerInstances
    {
        get
        {
            return RunState(() =>
            {
                if (_handlerActivationClosed)
                    throw new InvalidOperationException(
                        $"Actor '{ActorId}' handler activation is no longer available."
                    );
                return (
                    _handlerActivation ??= new ZLinkActorHandlerActivation(_services)
                ).Instances;
            });
        }
    }

    internal ValueTask DisposeHandlerActivationAsync()
    {
        ZLinkActorHandlerActivation? activation;
        activation = RunState(() =>
        {
            activation = _handlerActivation;
            _handlerActivation = null;
            return activation;
        });

        return activation?.DisposeAsync() ?? ValueTask.CompletedTask;
    }

    internal ZLinkActorHandlerTerminalCompletion<T> BeginHandlerActivationCompletion<T>(
        Func<T> terminalTransition
    )
    {
        ArgumentNullException.ThrowIfNull(terminalTransition);
        var requiresDispatchRelease = OwnsCurrentDispatch;
        return RunState(() =>
        {
            CloseHandlerActivationCore();
            var barrier = _dispatchMailbox.CloseAdmissionAndReserveLifecycleBarrier();
            Task<T> completion;
            using (ExecutionContext.SuppressFlow())
                completion = Task.Run(() =>
                    CompleteHandlerActivationCoreAsync(barrier, terminalTransition)
                );
            _terminalLifecycleCompletion = completion;
            return new ZLinkActorHandlerTerminalCompletion<T>(completion, requiresDispatchRelease);
        });
    }

    internal ZLinkActorHandlerTerminalCompletion<T> BeginHandlerActivationAsyncCompletion<T>(
        Func<ValueTask<T>> terminalTransition
    )
    {
        ArgumentNullException.ThrowIfNull(terminalTransition);
        var requiresDispatchRelease = OwnsCurrentDispatch;
        return RunState(() =>
        {
            CloseHandlerActivationCore();
            var barrier = _dispatchMailbox.CloseAdmissionAndReserveLifecycleBarrier();
            Task<T> completion;
            using (ExecutionContext.SuppressFlow())
                completion = Task.Run(() =>
                    CompleteHandlerActivationAsyncCoreAsync(barrier, terminalTransition)
                );
            _terminalLifecycleCompletion = completion;
            return new ZLinkActorHandlerTerminalCompletion<T>(completion, requiresDispatchRelease);
        });
    }

    internal bool OwnsCurrentDispatch =>
        AmbientDispatch.Value is { IsActive: true } ownership
        && ReferenceEquals(ownership.State, this);

    private async Task<T> CompleteHandlerActivationCoreAsync<T>(
        ZLinkActorSerialExecutor.BarrierReservation barrier,
        Func<T> terminalTransition
    )
    {
        try
        {
            using (await barrier.ClaimAsync().ConfigureAwait(false))
            {
                var result = await ExecuteLockedAsync(terminalTransition, CancellationToken.None)
                    .ConfigureAwait(false);
                await DisposeHandlerActivationAsync().ConfigureAwait(false);
                return result;
            }
        }
        finally
        {
            try
            {
                await barrier.Completion.ConfigureAwait(false);
            }
            finally
            {
                await _dispatchMailbox.ReleaseExecutionPlacementAsync().ConfigureAwait(false);
            }
        }
    }

    private async Task<T> CompleteHandlerActivationAsyncCoreAsync<T>(
        ZLinkActorSerialExecutor.BarrierReservation barrier,
        Func<ValueTask<T>> terminalTransition
    )
    {
        // The barrier is a drain fence for turns accepted before teardown.
        // Native terminal work may itself need Core lifecycle delivery, so it
        // must not run while the managed Actor mailbox turn remains claimed.
        try
        {
            using (await barrier.ClaimAsync().ConfigureAwait(false)) { }
            await barrier.Completion.ConfigureAwait(false);
            var result = await terminalTransition().ConfigureAwait(false);
            await DisposeHandlerActivationAsync().ConfigureAwait(false);
            return result;
        }
        finally
        {
            await _dispatchMailbox.ReleaseExecutionPlacementAsync().ConfigureAwait(false);
        }
    }

    internal ValueTask InvalidateRuntimeGenerationAfterDispatchesAsync() =>
        InvalidateRuntimeGenerationAfterDispatchesAsync(expectedActivation: null);

    internal ValueTask InvalidateRuntimeGenerationAfterDispatchesAsync(
        ZLinkSpotActivation? expectedActivation
    )
    {
        var completion = RunState(() =>
        {
            // The mailbox owns permanent disposal. Migration terminal completion
            // alone does not mean that this reusable Actor mailbox has ended.
            if (_dispatchMailbox.IsDisposed)
                return _terminalLifecycleCompletion!;
            if (expectedActivation is not null && !ReferenceEquals(Activation, expectedActivation))
                return Task.CompletedTask;

            FenceRuntimeGeneration();
            CloseHandlerActivationCore();
            var previousTerminal = _terminalLifecycleCompletion;
            var barrier = previousTerminal is null
                ? _dispatchMailbox.CloseAdmissionAndReserveLifecycleBarrier()
                : null;
            var mailboxDisposal = _dispatchMailbox.DisposeAsync().AsTask();
            Task terminal;
            using (ExecutionContext.SuppressFlow())
                terminal = Task.Run(() =>
                    CompleteRuntimeInvalidationAsync(previousTerminal, barrier, mailboxDisposal)
                );
            _terminalLifecycleCompletion = terminal;
            return terminal;
        });
        return new ValueTask(completion);
    }

    private async Task CompleteRuntimeInvalidationAsync(
        Task? previousTerminal,
        ZLinkActorSerialExecutor.BarrierReservation? barrier,
        Task mailboxDisposal
    )
    {
        var failures = new ZLinkFailureCollector();
        if (previousTerminal is not null)
            await failures
                .CaptureAsync(() => new ValueTask(previousTerminal))
                .ConfigureAwait(false);
        await failures
            .CaptureAsync(async () =>
            {
                using ZLinkActorSerialExecutor.Turn? turn = barrier is null
                    ? null
                    : await barrier.ClaimAsync().ConfigureAwait(false);
                await failures
                    .CaptureAsync(() =>
                        ExecuteLockedAsync(InvalidateRuntimeGenerationCore, CancellationToken.None)
                    )
                    .ConfigureAwait(false);
                await failures.CaptureAsync(DisposeHandlerActivationAsync).ConfigureAwait(false);
            })
            .ConfigureAwait(false);
        // Returning the barrier turn precedes joining the already closed mailbox.
        await failures.CaptureAsync(() => new ValueTask(mailboxDisposal)).ConfigureAwait(false);
        failures.ThrowIfAny();
    }

    private void CloseHandlerActivationCore()
    {
        _handlerActivationClosed = true;
    }

    public bool IsConfigured { get; private set; }

    public bool ContextInvalidated => Volatile.Read(ref _contextInvalidated) != 0;

    internal void FenceRuntimeGeneration() => Interlocked.Exchange(ref _contextInvalidated, 1);

    private volatile ZLinkActorDestroyPhase _destroyPhase;
    private volatile bool _reservedCreationPending;
    private int _deferredJoinPending;

    public bool IsDispatchBlocked =>
        _destroyPhase != ZLinkActorDestroyPhase.None || _reservedCreationPending;

    public bool IsTeardownPending => _destroyPhase != ZLinkActorDestroyPhase.None;

    public ZLinkActorSerialExecutor.BarrierReservation? ReserveDeferredJoinBarrier(
        out Task? targetCompletion
    )
    {
        targetCompletion = null;
        if (IsDispatchBlocked || Interlocked.CompareExchange(ref _deferredJoinPending, 1, 0) != 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{ActorId}' already has an active lifecycle transition."
            );

        try
        {
            targetCompletion = Handoff.BeginDeferredJoinCapture();
            if (targetCompletion is not null)
                return null;
            return _dispatchMailbox.ReserveBarrier();
        }
        catch
        {
            ReleaseDeferredJoinBarrier();
            throw;
        }
    }

    public ZLinkActorSerialExecutor.BarrierReservation ReserveDeferredJoinBarrierAfterTarget()
    {
        try
        {
            return _dispatchMailbox.ReserveBarrier();
        }
        catch
        {
            ReleaseDeferredJoinBarrier();
            throw;
        }
    }

    internal ZLinkActorSerialExecutor.BarrierReservation ReserveHandoffRestoreBarrier() =>
        _dispatchMailbox.ReserveBarrier();

    internal IReadOnlyList<Task> AdmitDurablePrefix(
        IReadOnlyList<ZLinkActorDurableRecord> records
    ) => _dispatchMailbox.AdmitDurablePrefix(records);

    public void EnsureDeferredJoinIdentity(IZLinkActor actor, ulong objectGeneration)
    {
        if (
            ContextInvalidated
            || !ReferenceEquals(Actor, actor)
            || NativeActorRef is not { Generation: var currentGeneration }
            || currentGeneration != objectGeneration
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{ActorId}' no longer matches the context that registered the Join."
            );
    }

    public void ReleaseDeferredJoinBarrier()
    {
        try
        {
            Handoff.EndDeferredJoinCapture(
                (frame, start) =>
                    _services
                        .GetRequiredService<ZLinkFrameworkRuntime>()
                        .AdmitActorHandoffFrame(this, frame, start)
            );
        }
        finally
        {
            Volatile.Write(ref _deferredJoinPending, 0);
            _dispatchMailbox.NotifyReadiness();
        }
    }

    public void BeginReservedCreation()
    {
        EnsureReusable();
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_reserved_begin actor={ActorId} actor_present={Actor is not null} native_generation={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(NativeActorRef?.Generation)} retired_generation={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(RetiredLocalActorRef?.Generation)} creation_task={_actorCreationTask is not null} context_invalidated={ContextInvalidated}"
        );
        _reservedCreationPending = true;
    }

    public void PublishReservedCreation()
    {
        _reservedCreationPending = false;
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_reserved_published actor={ActorId} actor_present={Actor is not null} native_generation={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(NativeActorRef?.Generation)}"
        );
    }

    public ZLinkSpotActivation? LiveActivation =>
        Activation is { IsDisposed: false } activation ? activation : null;

    public string? SpotId => LiveActivation?.SpotId;

    public void AttachStream(IZLinkStream stream)
    {
        SessionId = stream.SessionId;
        Stream = stream;
    }

    public bool DetachStreamIfCurrent(IZLinkStream stream)
    {
        if (!string.Equals(SessionId, stream.SessionId, StringComparison.Ordinal))
            return false;

        SessionId = null;
        Stream = null;
        return true;
    }

    public void JoinSpot(ZLinkSpotActivation activation)
    {
        Context?.UpdateSameNodeSpot(activation.SpotId);
        Activation = activation;
        if (activation.ExecutionMode == ZLinkUserSpotExecutionMode.SpotWide)
            _dispatchMailbox.BindExecutionOwner(() => Activation?._serial.ActorIngressOwner);
        EnsureActorMetric();
    }

    public void LeaveSpotIfCurrent(ZLinkSpotActivation activation)
    {
        if (!ReferenceEquals(Activation, activation))
            return;

        Context?.UpdateSameNodeSpot(null);
        Activation = null;
    }

    public void BindNativeActorRef(ZLinkBackendActorRef actorRef)
    {
        EnsureReusable();
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_native_bound actor={ActorId} generation={actorRef.Generation} node={actorRef.NodeRid} previous_generation={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(NativeActorRef?.Generation)}"
        );
        NativeActorRef = actorRef;
    }

    public bool BindActorInstance(IZLinkActor actor)
    {
        EnsureReusable();
        if (Context is { } expectedContext && !ReferenceEquals(actor.Context, expectedContext))
            throw new InvalidOperationException(
                $"Actor '{ActorId}' must expose its framework-issued Context."
            );

        if (
            Actor is not null
            && !ReferenceEquals(Actor, actor)
            && (SessionId is not null || Activation is not null)
        )
            throw new InvalidOperationException(
                $"Actor id '{ActorId}' is already bound to another actor instance."
            );

        if (ReferenceEquals(Actor, actor))
            return false;

        Actor = actor;
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_instance_bound actor={ActorId} native_generation={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(NativeActorRef?.Generation)} native_node={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(NativeActorRef?.NodeRid)}"
        );
        EnsureActorMetric();
        IsConfigured = false;
        return true;
    }

    public ZLinkActorContext GetOrCreateContext(Func<ZLinkActorContext> createContext)
    {
        EnsureReusable();
        if (Context is not null)
            return Context;

        Interlocked.Exchange(ref _contextInvalidated, 0);
        Context = createContext();
        EnsureActorMetric();
        return Context;
    }

    public bool TryBeginActorConfiguration()
    {
        if (IsConfigured)
            return false;

        IsConfigured = true;
        return true;
    }

    public void RollBackActorConfiguration(bool clearAssignedActor)
    {
        IsConfigured = false;
        if (clearAssignedActor)
        {
            Actor = null;
            ClearActorMetric();
        }
    }

    public ZLinkActorBoundSession? BindSession(
        RoutingId? sessionNodeRid,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration = 1,
        ulong objectGeneration = 0,
        ulong authorityOwnerGeneration = 0,
        ZLinkMeshName meshName = default,
        ulong targetNodeGeneration = 1,
        ulong ownerLeaseGeneration = 0,
        ulong sessionOwnerNodeGeneration = 1,
        ulong acceptedHighWater = 0,
        string sessionOwnerId = "",
        ulong sessionOwnerLeaseGeneration = 0
    )
    {
        var replacement = CreateSessionBinding(
            sessionNodeRid,
            sessionRid,
            bindingToken,
            bindingGeneration,
            objectGeneration,
            authorityOwnerGeneration,
            meshName,
            targetNodeGeneration,
            ownerLeaseGeneration,
            sessionOwnerNodeGeneration,
            acceptedHighWater,
            sessionOwnerId,
            sessionOwnerLeaseGeneration
        );
        return RunState<ZLinkActorBoundSession?>(() =>
        {
            PurgeExpiredSessionBindingTombstones();
            EnsureBindingTokenCanBeUsed(replacement);
            if (_boundSession is { } current)
            {
                if (SameBindingToken(current, replacement))
                {
                    _boundSession = replacement;
                    return current;
                }
                RememberRetiredSessionBinding(current);
            }
            _boundSession = replacement;
            return null;
        });
    }

    public ZLinkActorSessionBindingTransition ReplaceSessionBinding(
        RoutingId? sessionNodeRid,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkMeshName meshName,
        ulong targetNodeGeneration,
        ulong ownerLeaseGeneration,
        ulong sessionOwnerNodeGeneration,
        ulong acceptedHighWater,
        string sessionOwnerId = "",
        ulong sessionOwnerLeaseGeneration = 0,
        Action<ZLinkActorRuntimeState>? acceptAuthority = null,
        Action? beforePublish = null
    )
    {
        return RunState(() =>
        {
            acceptAuthority?.Invoke(this);
            var replacement = CreateSessionBinding(
                sessionNodeRid,
                sessionRid,
                bindingToken,
                bindingGeneration,
                objectGeneration,
                authorityOwnerGeneration,
                meshName,
                targetNodeGeneration,
                ownerLeaseGeneration,
                sessionOwnerNodeGeneration,
                acceptedHighWater,
                sessionOwnerId,
                sessionOwnerLeaseGeneration
            );

            PurgeExpiredSessionBindingTombstones();
            if (_boundSession is { } current)
            {
                if (
                    current.SessionNodeRid == replacement.SessionNodeRid
                    && current.SessionOwnerNodeGeneration == replacement.SessionOwnerNodeGeneration
                    && current.SessionRid == replacement.SessionRid
                    && current.BindingGeneration == replacement.BindingGeneration
                )
                {
                    beforePublish?.Invoke();
                    return new ZLinkActorSessionBindingTransition(current, null, Changed: false);
                }
            }
            if (
                _boundSession is { } registered
                && registered.SessionNodeRid == replacement.SessionNodeRid
                && registered.SessionOwnerNodeGeneration == replacement.SessionOwnerNodeGeneration
                && replacement.BindingGeneration <= registered.BindingGeneration
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{ActorId}' session binding generation is stale.",
                    ZLinkRetryAdvice.DoNotRetry
                );
            EnsureBindingTokenCanBeUsed(replacement);
            var previous = _boundSession;
            if (previous is { } retired)
                RememberRetiredSessionBinding(retired, beforePublish);
            else
                beforePublish?.Invoke();
            _boundSession = replacement;
            return new ZLinkActorSessionBindingTransition(replacement, previous, Changed: true);
        });
    }

    internal int SessionBindingTombstoneCount
    {
        get
        {
            return RunState(() =>
            {
                PurgeExpiredSessionBindingTombstones();
                return _sessionBindingTombstones.Count;
            });
        }
    }

    private ZLinkActorBoundSession CreateSessionBinding(
        RoutingId? sessionNodeRid,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkMeshName meshName,
        ulong targetNodeGeneration,
        ulong ownerLeaseGeneration,
        ulong sessionOwnerNodeGeneration,
        ulong acceptedHighWater,
        string sessionOwnerId,
        ulong sessionOwnerLeaseGeneration
    )
    {
        EnsureReusable();
        if (string.IsNullOrWhiteSpace(sessionOwnerId))
            sessionOwnerId = (sessionNodeRid ?? sessionRid).ToHex();
        if (sessionOwnerLeaseGeneration == 0)
            sessionOwnerLeaseGeneration = sessionOwnerNodeGeneration;
        if (bindingToken.Length == 0)
            throw new InvalidOperationException("Actor session binding token must not be empty.");
        if (
            sessionRid.IsEmpty
            || bindingGeneration == 0
            || objectGeneration == 0
            || authorityOwnerGeneration == 0
            || string.IsNullOrWhiteSpace(meshName.Value)
            || targetNodeGeneration == 0
            || ownerLeaseGeneration == 0
            || sessionOwnerNodeGeneration == 0
            || string.IsNullOrWhiteSpace(sessionOwnerId)
            || sessionOwnerId.Contains('\0')
            || sessionOwnerLeaseGeneration == 0
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{ActorId}' session binding requires an exact Actor, Session, Mesh, node lifecycle, authority, and owner lease."
            );

        return new ZLinkActorBoundSession(
            sessionNodeRid,
            sessionRid,
            bindingToken,
            bindingGeneration,
            objectGeneration,
            authorityOwnerGeneration,
            meshName,
            targetNodeGeneration,
            ownerLeaseGeneration,
            sessionOwnerNodeGeneration,
            acceptedHighWater,
            sessionOwnerId,
            sessionOwnerLeaseGeneration
        );
    }

    private void EnsureBindingTokenCanBeUsed(ZLinkActorBoundSession replacement)
    {
        if (!_sessionBindingTombstones.TryGetValue(replacement.BindingToken, out var tombstone))
            return;
        EnsureExactBindingIdentity(tombstone.Binding, replacement);
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"Actor '{ActorId}' session binding operation was already replaced.",
            ZLinkRetryAdvice.DoNotRetry
        );
    }

    private static bool SameBindingToken(
        ZLinkActorBoundSession left,
        ZLinkActorBoundSession right
    ) => string.Equals(left.BindingToken, right.BindingToken, StringComparison.Ordinal);

    private void EnsureExactBindingIdentity(
        ZLinkActorBoundSession expected,
        ZLinkActorBoundSession actual
    )
    {
        if (expected == actual)
            return;
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"Actor '{ActorId}' received conflicting fields for one session binding token.",
            ZLinkRetryAdvice.DoNotRetry
        );
    }

    private void RememberRetiredSessionBinding(
        ZLinkActorBoundSession binding,
        Action? beforePublish = null
    )
    {
        PurgeExpiredSessionBindingTombstones();
        if (
            !_sessionBindingTombstones.ContainsKey(binding.BindingToken)
            && _sessionBindingTombstones.Count >= _maxSessionBindingTombstones
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{ActorId}' session binding tombstone capacity is exhausted.",
                ZLinkRetryAdvice.RetryAfterBackoff
            );
        beforePublish?.Invoke();
        _sessionBindingTombstones[binding.BindingToken] = new ZLinkActorSessionBindingTombstone(
            binding,
            _timeProvider.GetElapsedTime(0, _timeProvider.GetTimestamp())
                + _sessionBindingTombstoneRetention
        );
    }

    private void PurgeExpiredSessionBindingTombstones()
    {
        if (_sessionBindingTombstones.Count == 0)
            return;
        var now = _timeProvider.GetElapsedTime(0, _timeProvider.GetTimestamp());
        foreach (
            var token in _sessionBindingTombstones
                .Where(entry => entry.Value.ExpiresAt <= now)
                .Select(static entry => entry.Key)
                .ToArray()
        )
            _sessionBindingTombstones.Remove(token);
    }

    public void RecordBoundSessionAccepted(string bindingToken)
    {
        RunState(() =>
        {
            if (
                _boundSession is not { } current
                || !string.Equals(current.BindingToken, bindingToken, StringComparison.Ordinal)
            )
                return;
            _boundSession = current with
            {
                AcceptedHighWater = checked(current.AcceptedHighWater + 1),
            };
        });
    }

    public void RecordRelocatedSessionAccepted(RoutingId sessionRid)
    {
        RunState(() =>
        {
            if (
                _pendingSessionRoute is { } pending
                && pending.Route.SessionRid is { } pendingSessionRid
                && pendingSessionRid == sessionRid
            )
            {
                _pendingSessionRoute = pending with
                {
                    Route = pending.Route with
                    {
                        AcceptedHighWater = checked(pending.Route.AcceptedHighWater + 1),
                    },
                };
                return;
            }

            if (_boundSession is not { } current || current.SessionRid != sessionRid)
                return;
            _boundSession = current with
            {
                AcceptedHighWater = checked(current.AcceptedHighWater + 1),
            };
        });
    }

    // A relayed frame carries the sequence assigned by the Session owner.
    // That value is authoritative across relay hops; incrementing it at
    // every hop counts one accepted frame more than once.
    public void RecordRelocatedSessionAccepted(RoutingId sessionRid, ulong acceptedHighWater)
    {
        if (acceptedHighWater == 0)
            return;

        RunState(() =>
        {
            if (
                _pendingSessionRoute is { } pending
                && pending.Route.SessionRid is { } pendingSessionRid
                && pendingSessionRid == sessionRid
            )
            {
                if (acceptedHighWater <= pending.Route.AcceptedHighWater)
                    return;
                _pendingSessionRoute = pending with
                {
                    Route = pending.Route with { AcceptedHighWater = acceptedHighWater },
                };
                return;
            }

            if (
                _boundSession is not { } current
                || current.SessionRid != sessionRid
                || acceptedHighWater <= current.AcceptedHighWater
            )
                return;
            _boundSession = current with { AcceptedHighWater = acceptedHighWater };
        });
    }

    public void StageRelocationSessionRoute(
        string handoffId,
        ZLinkRemoteActorBoundSessionRoute route,
        ZLinkSessionRelocationContext wireContext = default
    )
    {
        RunState(() =>
        {
            if (!route.IsBound)
            {
                _pendingSessionRoute = null;
                return;
            }
            if (
                _pendingSessionRoute is { } pending
                && !string.Equals(pending.HandoffId, handoffId, StringComparison.Ordinal)
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{ActorId}' already stages another session route."
                );
            _pendingSessionRoute = new ZLinkPendingActorSessionRoute(
                handoffId,
                route,
                TargetActor: null,
                TargetAuthorityOwnerGeneration: 0,
                WireContext: wireContext
            );
        });
    }

    internal void RememberSourceSessionRelocation(
        string handoffId,
        ZLinkSessionRelocationContext context
    )
    {
        RunState(() =>
        {
            if (
                _sourceSessionRelocation is { } current
                && (
                    !string.Equals(current.HandoffId, handoffId, StringComparison.Ordinal)
                    || current.Context != context
                )
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{ActorId}' already has another source session relocation.",
                    ZLinkRetryAdvice.DoNotRetry
                );
            _sourceSessionRelocation = new(handoffId, context);
        });
    }

    internal bool TryGetSourceSessionRelocation(
        string handoffId,
        out ZLinkSessionRelocationContext context
    )
    {
        var result = RunState(() =>
        {
            if (
                _sourceSessionRelocation is { } current
                && string.Equals(current.HandoffId, handoffId, StringComparison.Ordinal)
            )
            {
                return (true, current.Context);
            }
            return (false, default(ZLinkSessionRelocationContext));
        });
        context = result.Item2;
        return result.Item1;
    }

    internal void ForgetSourceSessionRelocation(string handoffId)
    {
        RunState(() =>
        {
            if (
                _sourceSessionRelocation is { } current
                && string.Equals(current.HandoffId, handoffId, StringComparison.Ordinal)
            )
                _sourceSessionRelocation = null;
        });
    }

    public void MarkRelocationSessionAuthorityCommitted(
        string handoffId,
        ZLinkBackendActorRef targetActor,
        ulong targetAuthorityOwnerGeneration,
        ZLinkMeshName targetMeshName,
        ulong targetNodeGeneration,
        ulong targetOwnerLeaseGeneration
    )
    {
        RunState(() =>
        {
            if (_pendingSessionRoute is not { } pending)
                return;
            if (
                !string.Equals(pending.HandoffId, handoffId, StringComparison.Ordinal)
                || pending.Route.ObjectGeneration != targetActor.Generation
                || targetMeshName.Value.Length == 0
                || targetNodeGeneration == 0
                || targetOwnerLeaseGeneration == 0
                || targetAuthorityOwnerGeneration <= pending.Route.AuthorityOwnerGeneration
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{ActorId}' staged session route does not match committed authority."
                );
            var committed = pending with
            {
                TargetActor = targetActor,
                TargetAuthorityOwnerGeneration = targetAuthorityOwnerGeneration,
                TargetMeshName = targetMeshName,
                TargetNodeGeneration = targetNodeGeneration,
                TargetOwnerLeaseGeneration = targetOwnerLeaseGeneration,
            };
            _pendingSessionRoute = committed;
        });
    }

    public bool TryGetCommittedRelocationSessionRoute(
        string handoffId,
        out ZLinkPendingActorSessionRoute route
    )
    {
        var result = RunState(() =>
        {
            if (
                _pendingSessionRoute
                    is { TargetActor: not null, TargetAuthorityOwnerGeneration: > 0 } pending
                && string.Equals(pending.HandoffId, handoffId, StringComparison.Ordinal)
            )
            {
                return (true, pending);
            }
            return (false, default(ZLinkPendingActorSessionRoute));
        });
        route = result.Item2;
        return result.Item1;
    }

    internal ZLinkServiceWireCodec.SessionRelocationRouteRecord CreateRelocationSessionRoute(
        string handoffId
    )
    {
        return RunState(() =>
        {
            if (
                _pendingSessionRoute
                    is not { TargetActor: not null, TargetAuthorityOwnerGeneration: > 0 } pending
                || !string.Equals(pending.HandoffId, handoffId, StringComparison.Ordinal)
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{ActorId}' has no committed session route for handoff '{handoffId}'.",
                    ZLinkRetryAdvice.DoNotRetry
                );
            return ZLinkSessionRelocationWire.CreateCommit(ActorId, pending);
        });
    }

    public bool TryGetStagedRelocationSessionRoute(
        string handoffId,
        out ZLinkRemoteActorBoundSessionRoute route
    )
    {
        var result = RunState(() =>
        {
            if (
                _pendingSessionRoute is { } pending
                && string.Equals(pending.HandoffId, handoffId, StringComparison.Ordinal)
            )
            {
                return (true, pending.Route);
            }
            return (false, default(ZLinkRemoteActorBoundSessionRoute));
        });
        route = result.Item2;
        return result.Item1;
    }

    internal bool TryGetAnyStagedRelocationSessionRoute(out ZLinkRemoteActorBoundSessionRoute route)
    {
        var result = RunState(() =>
        {
            if (_pendingSessionRoute is { } pending)
            {
                return (pending.Route.IsBound, pending.Route);
            }
            return (false, default(ZLinkRemoteActorBoundSessionRoute));
        });
        route = result.Item2;
        return result.Item1;
    }

    public void CompleteRelocationSessionRoute(string handoffId)
    {
        RunState(() =>
        {
            if (
                _pendingSessionRoute is not { } pending
                || !string.Equals(pending.HandoffId, handoffId, StringComparison.Ordinal)
            )
                return;
            _boundSession = CreateCommittedRelocationSession(pending);
            _pendingSessionRoute = null;
        });
    }

    public void AbortRelocationSessionRoute(string handoffId)
    {
        RunState(() =>
        {
            if (
                _pendingSessionRoute is { } pending
                && string.Equals(pending.HandoffId, handoffId, StringComparison.Ordinal)
            )
                _pendingSessionRoute = null;
        });
    }

    public void UnbindSession(string bindingToken)
    {
        if (bindingToken.Length == 0)
            return;

        RunState(() =>
        {
            if (
                _boundSession is { BindingToken: var current }
                && string.Equals(current, bindingToken, StringComparison.Ordinal)
            )
            {
                _boundSession = null;
            }
            else if (
                _pendingSessionRoute is { Route.BindingToken: var pendingToken }
                && string.Equals(pendingToken, bindingToken, StringComparison.Ordinal)
            )
            {
                // Exact disconnect also retires a staged relocation route.
                _pendingSessionRoute = null;
            }
        });
    }

    public void TombstoneSession(
        ZLinkActorBoundSession expected,
        Action<ZLinkActorRuntimeState>? acceptAuthority = null,
        Action? reply = null
    )
    {
        RunState(() =>
        {
            acceptAuthority?.Invoke(this);
            PurgeExpiredSessionBindingTombstones();
            if (_sessionBindingTombstones.TryGetValue(expected.BindingToken, out var tombstone))
            {
                reply?.Invoke();
                return;
            }
            RememberRetiredSessionBinding(expected);
            if (_boundSession is { } current && SameBindingToken(current, expected))
                _boundSession = null;
            reply?.Invoke();
        });
    }

    public bool TryGetBoundSession(out ZLinkActorBoundSession session)
    {
        var result = RunState(() =>
        {
            if (_boundSession is { } current)
            {
                return (true, current);
            }
            return (false, default(ZLinkActorBoundSession));
        });
        session = result.Item2;
        return result.Item1;
    }

    internal bool TryGetBoundSessionForOutbound(out ZLinkActorBoundSession session)
    {
        var result = RunState(() =>
        {
            // After the target authority commits, _boundSession keeps the
            // source projection until the target has submitted the one-way
            // Session route update (relocation flow §4.6; no reply follows).
            // Outbound pushes in that interval use the committed target
            // projection; otherwise the first target push is fenced as a
            // stale source push.
            if (
                _pendingSessionRoute is
                { TargetActor: not null, TargetAuthorityOwnerGeneration: > 0 } pending
            )
            {
                return (true, CreateCommittedRelocationSession(pending));
            }
            if (_boundSession is { } current)
            {
                return (true, current);
            }
            return (false, default(ZLinkActorBoundSession));
        });
        session = result.Item2;
        return result.Item1;
    }

    internal bool TryGetBoundSessionForInbound(out ZLinkActorBoundSession session)
    {
        var result = RunState(() =>
        {
            // During relocation, the target accepts frames before the staged
            // route is promoted to _boundSession. Use the committed target
            // projection while retaining the source session identity fence.
            if (
                _pendingSessionRoute is
                { TargetActor: not null, TargetAuthorityOwnerGeneration: > 0 } pending
            )
            {
                return (true, CreateCommittedRelocationSession(pending));
            }
            if (_boundSession is { } current)
            {
                return (true, current);
            }
            return (false, default(ZLinkActorBoundSession));
        });
        session = result.Item2;
        return result.Item1;
    }

    private static ZLinkActorBoundSession CreateCommittedRelocationSession(
        ZLinkPendingActorSessionRoute pending
    )
    {
        var target =
            pending.TargetActor
            ?? throw new InvalidOperationException(
                "A session route cannot complete before authority commit."
            );
        var route = pending.Route;
        return new ZLinkActorBoundSession(
            route.NodeRid,
            route.SessionRid!.Value,
            route.BindingToken!,
            route.BindingGeneration,
            target.Generation,
            pending.TargetAuthorityOwnerGeneration,
            pending.TargetMeshName
                ?? throw new InvalidOperationException(
                    "A committed session route must have a target mesh."
                ),
            pending.TargetNodeGeneration,
            pending.TargetOwnerLeaseGeneration,
            route.SessionOwnerNodeGeneration,
            route.AcceptedHighWater,
            route.SessionOwnerId ?? "",
            route.SessionOwnerLeaseGeneration
        );
    }

    public bool TryUseBoundSession(
        string expectedBindingToken,
        Func<ZLinkActorBoundSession, bool> operation
    )
    {
        ArgumentNullException.ThrowIfNull(operation);

        return RunState(() =>
        {
            if (
                _boundSession is not { } current
                || !string.Equals(
                    current.BindingToken,
                    expectedBindingToken,
                    StringComparison.Ordinal
                )
            )
                return true;

            return operation(current);
        });
    }

    public ZLinkActorBoundSession? ClearAfterDestroy() =>
        RunState(() => TransitionLocalInstanceCore(ZLinkActorTerminalTransition.Destroyed, null));

    internal ZLinkActorBoundSession? ClearAfterDestroyOnLane() =>
        TransitionLocalInstanceCore(ZLinkActorTerminalTransition.Destroyed, null);

    public void InvalidateRuntimeGeneration() => RunState(InvalidateRuntimeGenerationCore);

    private void InvalidateRuntimeGenerationCore()
    {
        var failure = new InvalidOperationException(
            $"Actor '{ActorId}' belongs to a stopped framework runtime generation."
        );
        var teardownAttempt = _teardownAttempt;
        Handoff.AbortRuntimeGeneration(failure);
        _ = TransitionLocalInstanceCore(ZLinkActorTerminalTransition.Destroyed, null);
        teardownAttempt?.TrySetResult(failure);
    }

    public void RetireMigratedActorInstance(ZLinkBackendActorRef sourceActor) =>
        RunState(() => RetireMigratedActorInstanceCore(sourceActor));

    internal void RetireMigratedActorInstanceOnLane(ZLinkBackendActorRef sourceActor) =>
        RetireMigratedActorInstanceCore(sourceActor);

    private void RetireMigratedActorInstanceCore(ZLinkBackendActorRef sourceActor)
    {
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_migrated actor={ActorId} source_generation={sourceActor.Generation} current_generation={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(NativeActorRef?.Generation)}"
        );
        _ = TransitionLocalInstanceCore(ZLinkActorTerminalTransition.Migrated, sourceActor);
    }

    private ZLinkActorBoundSession? TransitionLocalInstanceCore(
        ZLinkActorTerminalTransition transition,
        ZLinkBackendActorRef? retiredLocalActor
    )
    {
        CloseHandlerActivationCore();
        ZLinkActorBoundSession? releasedBoundSession = null;
        if (transition == ZLinkActorTerminalTransition.Destroyed)
        {
            releasedBoundSession = _boundSession;
            _boundSession = null;
            _sessionBindingTombstones.Clear();
        }
        SessionId = null;
        Stream = null;
        Activation = null;
        Context = null;
        Actor = null;
        ClearActorMetric();
        ActorType = null;
        IsConfigured = false;
        FenceRuntimeGeneration();
        _actorCreationTask = null;

        switch (transition)
        {
            case ZLinkActorTerminalTransition.Destroyed:
                NativeActorRef = null;
                RetiredLocalActorRef = null;
                _destroyPhase = ZLinkActorDestroyPhase.None;
                _teardownAttempt = null;
                Handoff.Reset();
                _pendingSessionRoute = null;
                break;
            case ZLinkActorTerminalTransition.Migrated:
                RetiredLocalActorRef =
                    retiredLocalActor ?? throw new ArgumentNullException(nameof(retiredLocalActor));
                // A source migration keeps the old native reference only in
                // RetiredLocalActorRef for delayed cleanup. If the current
                // binding is that source reference, it must not be reused by
                // the next reserved creation; a target reference that was
                // already staged remains the current binding.
                if (NativeActorRef == RetiredLocalActorRef)
                    NativeActorRef = null;
                Handoff.CompleteSourceMigration();
                break;
            default:
                throw new InvalidOperationException("Unknown actor terminal transition.");
        }

        return releasedBoundSession;
    }

    private void ClearActorMetric()
    {
        if (Interlocked.Exchange(ref _actorMetricActive, 0) != 0)
            ZLinkRuntimeMetrics.RecordActorClosed(
                Interlocked.Exchange(ref _actorMetricMeshName, null)!
            );
    }

    private void EnsureActorMetric()
    {
        if (Actor is null || Volatile.Read(ref _actorMetricActive) != 0)
            return;
        var meshName = MeshName;
        if (string.IsNullOrWhiteSpace(meshName))
            return;
        if (Interlocked.CompareExchange(ref _actorMetricActive, 1, 0) != 0)
            return;
        _actorMetricMeshName = meshName;
        ZLinkRuntimeMetrics.RecordActorCreated(meshName);
    }

    private enum ZLinkActorTerminalTransition
    {
        Destroyed,
        Migrated,
    }

    private sealed class EmptyServiceProvider : IServiceProvider
    {
        public object? GetService(Type serviceType) => null;
    }

    internal async ValueTask PrepareForTransferredActivationAsync(
        CancellationToken cancellationToken
    )
    {
        var terminal = await ExecuteLockedAsync(
                () => _terminalLifecycleCompletion,
                cancellationToken
            )
            .ConfigureAwait(false);
        if (terminal is not null)
            // Framework API §11: source retirement publishes its handoff result
            // before async handler disposal finishes. Reentry needs that existing
            // terminal fence; the source finalizer owns reporting disposal errors.
            await Task.WhenAny(terminal).WaitAsync(cancellationToken).ConfigureAwait(false);

        await ExecuteLockedAsync(PrepareForTransferredActivationCore, cancellationToken)
            .ConfigureAwait(false);
    }

    private void PrepareForTransferredActivationCore()
    {
        if (Actor is not null || IsConfigured)
            throw new InvalidOperationException(
                $"Actor '{ActorId}' already has an active local instance."
            );

        Handoff.PrepareForTransferredActivation();
        NativeActorRef = null;
        Interlocked.Exchange(ref _contextInvalidated, 0);
        if (_terminalLifecycleCompletion is { IsCompleted: false })
            throw new InvalidOperationException(
                $"Actor '{ActorId}' still has a pending terminal lifecycle completion."
            );

        _dispatchMailbox.ReopenAdmission();
        _terminalLifecycleCompletion = null;
        _handlerActivationClosed = false;
    }

    public void ClearRetiredLocalActorRef(ZLinkBackendActorRef actor)
    {
        if (RetiredLocalActorRef == actor)
            RetiredLocalActorRef = null;
    }

    internal ZLinkActorTeardownOperation BeginOrJoinTeardownAttemptOnLane(
        ZLinkBackendActorRef nativeActor
    )
    {
        // Actor model §6.8: an absent incarnation is idempotent; a replacement
        // must never be selected by a stale cleanup request.
        if (NativeActorRef is null)
            return new ZLinkActorTeardownOperation(Task.FromResult<Exception?>(null), false, false);
        if (NativeActorRef != nativeActor)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{ActorId}' teardown does not match the current incarnation."
            );
        if (_teardownAttempt is { } existing)
            return new ZLinkActorTeardownOperation(existing.Task, false, false);

        CloseHandlerActivationCore();
        FenceRuntimeGeneration();
        var nativeAlreadyDestroyed = false;
        switch (_destroyPhase)
        {
            case ZLinkActorDestroyPhase.None:
            case ZLinkActorDestroyPhase.DestroyingNative:
                _destroyPhase = ZLinkActorDestroyPhase.DestroyingNative;
                break;
            case ZLinkActorDestroyPhase.NativeDestroyed:
                _destroyPhase = ZLinkActorDestroyPhase.ReleasingOwnership;
                nativeAlreadyDestroyed = true;
                break;
            default:
                throw new InvalidOperationException(
                    $"Actor '{ActorId}' teardown phase has no owning operation."
                );
        }

        _teardownAttempt = new TaskCompletionSource<Exception?>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        return new ZLinkActorTeardownOperation(_teardownAttempt.Task, true, nativeAlreadyDestroyed);
    }

    public void MarkNativeDestroyed(ZLinkActorTeardownOperation operation)
    {
        EnsureCurrentTeardownAttempt(operation);
        if (_destroyPhase != ZLinkActorDestroyPhase.DestroyingNative)
            throw new InvalidOperationException($"Actor '{ActorId}' native destroy is not active.");
        _destroyPhase = ZLinkActorDestroyPhase.ReleasingOwnership;
    }

    public void FailTeardownAttempt(
        ZLinkActorTeardownOperation operation,
        bool nativeDestroyed,
        Exception failure
    )
    {
        EnsureCurrentTeardownAttempt(operation);
        if (nativeDestroyed)
            _destroyPhase = ZLinkActorDestroyPhase.NativeDestroyed;
        else
            _destroyPhase = ZLinkActorDestroyPhase.DestroyingNative;
        var completion = _teardownAttempt!;
        _teardownAttempt = null;
        completion.TrySetResult(failure);
    }

    internal ZLinkActorBoundSession? CompleteTeardownAttemptOnLane(
        ZLinkActorTeardownOperation operation
    ) => CompleteTeardownAttemptCore(operation);

    private ZLinkActorBoundSession? CompleteTeardownAttemptCore(
        ZLinkActorTeardownOperation operation
    )
    {
        EnsureCurrentTeardownAttempt(operation);
        var completion = _teardownAttempt!;
        var boundSession = ClearAfterDestroyOnLane();
        completion.TrySetResult(null);
        return boundSession;
    }

    private void EnsureCurrentTeardownAttempt(ZLinkActorTeardownOperation operation)
    {
        if (
            _teardownAttempt is null
            || !ReferenceEquals(_teardownAttempt.Task, operation.Completion)
        )
            throw new InvalidOperationException(
                $"Actor '{ActorId}' teardown operation is no longer current."
            );
    }

    public void EnsureContextValid()
    {
        if (ContextInvalidated)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor context for '{ActorId}' cannot start an operation after owner cutover."
            );
    }

    public void InvalidateContext()
    {
        FenceRuntimeGeneration();
        Activation = null;
    }

    public ZLinkActorPlacementSelection SelectPlacementLocked(bool pruneWhenSessionless)
    {
        var currentActivation = Activation;
        var prune = false;
        if (currentActivation is not null && currentActivation.IsDisposed)
        {
            Activation = null;
            currentActivation = null;
            prune = pruneWhenSessionless && SessionId is null;
        }

        return new ZLinkActorPlacementSelection(currentActivation, prune);
    }

    public async ValueTask<ZLinkActorCreationOperation> GetOrStartActorCreationAsync(
        string actorType,
        bool failIfExists,
        Func<Task<IZLinkActor>> createActor,
        CancellationToken cancellationToken
    )
    {
        var created = false;
        var task = await ExecuteLockedAsync(
                () =>
                {
                    EnsureReusable();
                    if (
                        ActorType is not null
                        && !string.Equals(ActorType, actorType, StringComparison.Ordinal)
                    )
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.TypeMismatch,
                            $"Actor '{ActorId}' already uses actor type '{ActorType}', not '{actorType}'."
                        );

                    if (Actor is not null && ContextInvalidated)
                    {
                        Actor = null;
                        Context = null;
                        NativeActorRef = null;
                        IsConfigured = false;
                        Interlocked.Exchange(ref _contextInvalidated, 0);
                    }
                    else if (Actor is null && ContextInvalidated && _actorCreationTask is null)
                    {
                        // A migrated source retains the target reference while
                        // Message Follow can still drain. Once a new local
                        // creation starts, that reference is no longer a local
                        // materialization and must not be reused as its native
                        // binding.
                        Context = null;
                        NativeActorRef = null;
                        IsConfigured = false;
                        Interlocked.Exchange(ref _contextInvalidated, 0);
                    }

                    if (Actor is not null)
                    {
                        if (failIfExists)
                            throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.AlreadyExists,
                                $"Actor '{ActorId}' already exists."
                            );

                        return Task.FromResult(Actor);
                    }

                    if (_actorCreationTask is null)
                    {
                        ActorType = actorType;
                        created = true;
                        using (ExecutionContext.SuppressFlow())
                            _actorCreationTask = Task.Run(createActor);
                        var creationTask = _actorCreationTask!;
                        using (ExecutionContext.SuppressFlow())
                            _ = Task.Run(() =>
                                ClearActorCreationTaskWhenCompletedAsync(creationTask)
                            );
                    }
                    else if (failIfExists)
                    {
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.AlreadyExists,
                            $"Actor '{ActorId}' is already being created."
                        );
                    }

                    return _actorCreationTask;
                },
                cancellationToken
            )
            .ConfigureAwait(false);

        return new ZLinkActorCreationOperation(task, created);
    }

    public async ValueTask ExecuteLockedAsync(Action operation, CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await _lane.RunAsync(operation).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    public async ValueTask<T> ExecuteLockedAsync<T>(
        Func<T> operation,
        CancellationToken cancellationToken
    )
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return await _lane.RunAsync(operation).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    internal async ValueTask<ZLinkSpotActivation?> CommitSpotMembershipAsync(
        ZLinkSpotActivation activation,
        Func<CancellationToken, ValueTask> commitAuthority,
        Action publishTargetMembership,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(commitAuthority);
        ArgumentNullException.ThrowIfNull(publishTargetMembership);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var previousActivation = await _lane.RunAsync(() => Activation).ConfigureAwait(false);
            if (ReferenceEquals(previousActivation, activation))
                return previousActivation;
            await commitAuthority(cancellationToken).ConfigureAwait(false);
            await _lane
                .RunAsync(() =>
                {
                    JoinSpot(activation);
                    publishTargetMembership();
                })
                .ConfigureAwait(false);
            return previousActivation;
        }
        finally
        {
            _gate.Release();
        }
    }

    public async ValueTask<T> ExecuteHandoffTransitionAsync<T>(
        Func<T> transition,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(transition);
        //  An Actor that migrated away left this mailbox closed. Reopen before
        //  entering so a handoff bringing it back is not rejected by the state
        //  it left; EnsureReusable below still refuses a state in teardown.
        _dispatchMailbox.TryReopenAdmissionForIncomingHandoff();
        using var turn = await _dispatchMailbox.EnterAsync(cancellationToken).ConfigureAwait(false);
        return await ExecuteLockedAsync(
                () =>
                {
                    EnsureReusable();
                    return transition();
                },
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    public async ValueTask<int> BeginHandoffCaptureAsync(CancellationToken cancellationToken)
    {
        if (
            AmbientDispatch.Value is { IsActive: true } ownership
            && ReferenceEquals(ownership.State, this)
        )
        {
            await Handoff.WithdrawDeferredSuffixAsync().ConfigureAwait(false);
            return await ExecuteLockedAsync(
                    () =>
                    {
                        EnsureReusable();
                        var pendingRequests = _dispatchMailbox.PendingRequestCount;
                        Handoff.BeginCapture();
                        return pendingRequests;
                    },
                    cancellationToken
                )
                .ConfigureAwait(false);
        }

        using var turn = await _dispatchMailbox
            .EnterLifecycleAsync(cancellationToken)
            .ConfigureAwait(false);
        await Handoff.WithdrawDeferredSuffixAsync().ConfigureAwait(false);
        return await ExecuteLockedAsync(
                () =>
                {
                    EnsureReusable();
                    var pendingRequests = _dispatchMailbox.PendingRequestCount;
                    Handoff.BeginCapture();
                    return pendingRequests;
                },
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    public async ValueTask ExecuteDispatchAsync(
        Func<CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken
    )
    {
        using var turn = await _dispatchMailbox.EnterAsync(cancellationToken).ConfigureAwait(false);
        EnsureDispatchAvailable();
        using var dispatch = EnterDispatch();
        await operation(cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<T> ExecuteDispatchAsync<T>(
        Func<CancellationToken, ValueTask<T>> operation,
        bool countAsPendingRequest,
        CancellationToken cancellationToken,
        bool allowRelocationReplay = false,
        ZLinkSpotRelocationActorQueueReservation? replayReservation = null
    )
    {
        if (replayReservation is not null)
        {
            T result = default!;
            await replayReservation
                .ExecuteAsync(
                    ActorId,
                    async ct =>
                    {
                        EnsureDispatchAvailable(allowRelocationReplay);
                        using var dispatch = EnterDispatch();
                        result = await operation(ct).ConfigureAwait(false);
                    },
                    cancellationToken
                )
                .ConfigureAwait(false);
            return result;
        }
        using var turn = await _dispatchMailbox
            .EnterAsync(cancellationToken, countAsPendingRequest)
            .ConfigureAwait(false);
        EnsureDispatchAvailable(allowRelocationReplay);
        using var dispatch = EnterDispatch();
        return await operation(cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask ExecuteLifecycleAsync(
        Func<CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken
    )
    {
        using var turn = await _dispatchMailbox
            .EnterLifecycleAsync(cancellationToken)
            .ConfigureAwait(false);
        EnsureDispatchAvailable();
        await operation(cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask ExecuteRelocationCompletionAsync(
        ulong objectGeneration,
        Func<CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken
    )
    {
        using var turn = await _dispatchMailbox
            .EnterLifecycleAsync(cancellationToken)
            .ConfigureAwait(false);
        if (
            ContextInvalidated
            || NativeActorRef is not { Generation: var currentGeneration }
            || currentGeneration != objectGeneration
            || Actor is null
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{ActorId}' no longer matches its durable Join completion."
            );
        await operation(cancellationToken).ConfigureAwait(false);
    }

    private void EnsureDispatchAvailable(bool allowRelocationReplay = false)
    {
        if (!IsDispatchBlocked && (allowRelocationReplay || !Handoff.BlocksLocalDispatch))
            return;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"Actor '{ActorId}' is not available while its lifecycle transition is active."
        );
    }

    private void EnsureReusable()
    {
        // Reserved creation deliberately blocks public dispatch until the
        // authority Ready commit, but the factory, context and native Actor
        // binding that prepare that reservation must still mutate this state.
        if (_destroyPhase == ZLinkActorDestroyPhase.None)
            return;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"Actor '{ActorId}' cannot be reused while teardown is incomplete."
        );
    }

    public DispatchScope EnterDispatch()
    {
        var previousAmbient = AmbientDispatch.Value;
        var ownership = new DispatchOwnership(this);
        AmbientDispatch.Value = ownership;
        return new DispatchScope(previousAmbient, ownership);
    }

    public DispatchScope EnterDeferredJoinExecution() => EnterDispatch();

    public DispatchScope EnterForwardedDispatchExecution() => EnterDispatch();

    private async Task ClearActorCreationTaskWhenCompletedAsync(Task<IZLinkActor> creationTask)
    {
        var succeeded = true;
        try
        {
            await creationTask.ConfigureAwait(false);
        }
        catch
        {
            succeeded = false;
        }

        await ExecuteLockedAsync(
                () =>
                {
                    if (ReferenceEquals(_actorCreationTask, creationTask))
                    {
                        if (succeeded)
                            _actorCreationTask = null;
                        else
                            ClearFailedActorCreationLocked();
                    }
                },
                CancellationToken.None
            )
            .ConfigureAwait(false);
    }

    private void ClearFailedActorCreationLocked()
    {
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_creation_failed_clear actor={ActorId} native_generation={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(NativeActorRef?.Generation)} actor_present={Actor is not null} teardown={IsTeardownPending}"
        );
        _actorCreationTask = null;
        if (IsTeardownPending)
            return;

        Actor = null;
        ActorType = null;
        Context = null;
        Activation = null;
        NativeActorRef = null;
        IsConfigured = false;
        FenceRuntimeGeneration();
    }

    public readonly struct DispatchScope : IDisposable
    {
        private readonly DispatchOwnership? _previousAmbient;
        private readonly DispatchOwnership? _ownership;

        internal DispatchScope(DispatchOwnership? previousAmbient, DispatchOwnership ownership)
        {
            _previousAmbient = previousAmbient;
            _ownership = ownership;
        }

        public void Dispose()
        {
            if (_ownership is not null)
            {
                _ownership.Deactivate();
                AmbientDispatch.Value = _previousAmbient;
            }
        }
    }

    internal sealed class DispatchOwnership(ZLinkActorRuntimeState state)
    {
        private int _active = 1;

        public ZLinkActorRuntimeState State { get; } = state;

        public bool IsActive => Volatile.Read(ref _active) != 0;

        public void Deactivate() => Interlocked.Exchange(ref _active, 0);
    }
}

internal enum ZLinkActorDestroyPhase
{
    None,
    DestroyingNative,
    NativeDestroyed,
    ReleasingOwnership,
}

internal readonly record struct ZLinkActorTeardownOperation(
    Task<Exception?> Completion,
    bool OwnsExecution,
    bool NativeAlreadyDestroyed
);

internal readonly record struct ZLinkActorHandlerTerminalCompletion<T>(
    Task<T> Completion,
    bool RequiresDispatchRelease
);

// Actor-side delivery projection reconstructed from binding and relocation
// payloads. It does not publish or replace the Session owner's binding route.
internal readonly record struct ZLinkActorBoundSession(
    RoutingId? SessionNodeRid,
    RoutingId SessionRid,
    string BindingToken,
    ulong BindingGeneration = 1,
    ulong ObjectGeneration = 0,
    ulong AuthorityOwnerGeneration = 0,
    ZLinkMeshName MeshName = default,
    ulong TargetNodeGeneration = 1,
    ulong OwnerLeaseGeneration = 0,
    ulong SessionOwnerNodeGeneration = 1,
    ulong AcceptedHighWater = 0,
    string SessionOwnerId = "",
    ulong SessionOwnerLeaseGeneration = 0
)
{
    internal bool IsSamePhysicalSessionOwner(
        RoutingId sessionNodeRid,
        ulong sessionOwnerNodeGeneration,
        RoutingId sessionRid,
        string sessionOwnerId,
        ulong sessionOwnerLeaseGeneration
    )
    {
        if (SessionNodeRid is not { } previousNodeRid)
            return false;
        return previousNodeRid == sessionNodeRid
            && SessionOwnerNodeGeneration == sessionOwnerNodeGeneration
            && SessionRid == sessionRid
            && string.Equals(SessionOwnerId, sessionOwnerId, StringComparison.Ordinal)
            && SessionOwnerLeaseGeneration == sessionOwnerLeaseGeneration;
    }
}

internal readonly record struct ZLinkActorSessionBindingTransition(
    ZLinkActorBoundSession Replacement,
    ZLinkActorBoundSession? Previous,
    bool Changed
);

internal readonly record struct ZLinkActorSessionBindingTombstone(
    ZLinkActorBoundSession Binding,
    TimeSpan ExpiresAt
);

internal readonly record struct ZLinkPendingActorSessionRoute(
    string HandoffId,
    ZLinkRemoteActorBoundSessionRoute Route,
    ZLinkBackendActorRef? TargetActor,
    ulong TargetAuthorityOwnerGeneration,
    ZLinkMeshName? TargetMeshName = null,
    ulong TargetNodeGeneration = 0,
    ulong TargetOwnerLeaseGeneration = 0,
    ZLinkSessionRelocationContext WireContext = default
);

internal readonly record struct ZLinkSourceSessionRelocation(
    string HandoffId,
    ZLinkSessionRelocationContext Context
);

internal readonly record struct ZLinkActorCreationOperation(Task<IZLinkActor> Task, bool Created);

internal readonly record struct ZLinkActorPlacementSelection(
    ZLinkSpotActivation? Activation,
    bool Prune
);
