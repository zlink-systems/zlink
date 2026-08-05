using Zlink.Framework.Runtime.Handlers;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorRuntimeState(
    string actorId,
    TimeProvider? timeProvider = null,
    Action<string>? handoffDiagnostic = null,
    ZLinkBoundedIngressAdmission? sourceIngressAdmission = null,
    TimeSpan? sessionBindingTombstoneRetention = null,
    int maxSessionBindingTombstones = 1_024,
    IServiceProvider? services = null)
{
    private static readonly IServiceProvider EmptyServices =
        new EmptyServiceProvider();
    private static readonly TimeSpan DefaultSessionBindingTombstoneRetention =
        TimeSpan.FromMinutes(2);
    private static readonly AsyncLocal<DispatchOwnership?> AmbientDispatch = new();
    private readonly ZLinkActorDispatchMailbox _dispatchMailbox = new();
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly object _terminalLifecycleGate = new();
    private readonly object _sessionGate = new();
    private readonly TimeProvider _timeProvider = timeProvider ?? TimeProvider.System;
    private readonly TimeSpan _sessionBindingTombstoneRetention =
        sessionBindingTombstoneRetention is { } configured
        && configured > TimeSpan.Zero
            ? configured
            : DefaultSessionBindingTombstoneRetention;
    private readonly int _maxSessionBindingTombstones =
        maxSessionBindingTombstones > 0
            ? maxSessionBindingTombstones
            : 1_024;
    private readonly Dictionary<string, ZLinkActorSessionBindingTombstone>
        _sessionBindingTombstones = new(StringComparer.Ordinal);
    private Task<IZLinkActor>? _actorCreationTask;
    private int _actorMetricActive;
    private string? _actorMetricMeshName;
    private ZLinkActorBoundSession? _boundSession;
    private SessionBindingReplacement? _sessionReplacement;
    private ZLinkPendingActorSessionRoute? _pendingSessionRoute;
    private TaskCompletionSource<Exception?>? _teardownAttempt;
    private readonly IServiceProvider _services = services ?? EmptyServices;
    private ZLinkActorHandlerActivation? _handlerActivation;
    private bool _handlerActivationClosed;
    private Task? _terminalLifecycleCompletion;
    private int _contextInvalidated;

    public string ActorId { get; } = actorId;

    public ZLinkActorHandoffState Handoff { get; } = new(
        actorId,
        timeProvider ?? TimeProvider.System,
        handoffDiagnostic,
        sourceIngressAdmission);

    public string? ActorType { get; private set; }

    public string? SessionId { get; private set; }

    public IZLinkStream? Stream { get; private set; }

    public ZLinkBackendActorRef? NativeActorRef { get; private set; }

    public ZLinkBackendActorRef? RetiredLocalActorRef { get; private set; }

    public ZLinkSpotActivation? Activation { get; private set; }

    public ZLinkActorDispatchState? CurrentDispatch { get; private set; }

    public ZLinkActorContext? Context { get; private set; }

    public IZLinkActor? Actor { get; private set; }

    internal ZLinkScopedHandlerInstanceOwner HandlerInstances
    {
        get
        {
            lock (_sessionGate)
            {
                if (_handlerActivationClosed)
                    throw new InvalidOperationException(
                        $"Actor '{ActorId}' handler activation is no longer available.");
                return (_handlerActivation ??= new ZLinkActorHandlerActivation(_services))
                    .Instances;
            }
        }
    }

    internal ValueTask DisposeHandlerActivationAsync()
    {
        ZLinkActorHandlerActivation? activation;
        lock (_sessionGate)
        {
            activation = _handlerActivation;
            _handlerActivation = null;
        }

        return activation?.DisposeAsync() ?? ValueTask.CompletedTask;
    }

    internal ZLinkActorHandlerTerminalCompletion<T>
        BeginHandlerActivationCompletion<T>(
        Func<T> terminalTransition)
    {
        ArgumentNullException.ThrowIfNull(terminalTransition);
        lock (_terminalLifecycleGate)
        {
            CloseHandlerActivation();
            var requiresDispatchRelease =
                AmbientDispatch.Value is { IsActive: true } ownership
                && ReferenceEquals(ownership.State, this);
            var barrier =
                _dispatchMailbox.CloseAdmissionAndReserveLifecycleBarrier();
            var completion = CompleteHandlerActivationCoreAsync(
                barrier,
                terminalTransition);
            _terminalLifecycleCompletion = completion;
            return new ZLinkActorHandlerTerminalCompletion<T>(
                completion,
                requiresDispatchRelease);
        }
    }

    private async Task<T> CompleteHandlerActivationCoreAsync<T>(
        ZLinkActorDispatchMailbox.BarrierReservation barrier,
        Func<T> terminalTransition)
    {
        using var turn = await barrier.ClaimAsync().ConfigureAwait(false);
        var result = await ExecuteLockedAsync(
                terminalTransition,
                CancellationToken.None)
            .ConfigureAwait(false);
        await DisposeHandlerActivationAsync().ConfigureAwait(false);
        return result;
    }

    internal async ValueTask InvalidateRuntimeGenerationAfterDispatchesAsync()
    {
        Task? terminalCompletion;
        ZLinkActorDispatchMailbox.BarrierReservation? barrier = null;
        lock (_terminalLifecycleGate)
        {
            CloseHandlerActivation();
            terminalCompletion = _terminalLifecycleCompletion;
            if (terminalCompletion is null)
                barrier = _dispatchMailbox
                    .CloseAdmissionAndReserveLifecycleBarrier();
        }

        if (terminalCompletion is not null)
        {
            // A migration or teardown may have already closed admission and
            // be waiting for the current dispatch to return. Reusing that
            // completion keeps shutdown from reserving a second terminal
            // barrier for the same actor.
            await terminalCompletion.ConfigureAwait(false);
            await ExecuteLockedAsync(
                    InvalidateRuntimeGeneration,
                    CancellationToken.None)
                .ConfigureAwait(false);
            await DisposeHandlerActivationAsync().ConfigureAwait(false);
            return;
        }

        using var turn = await barrier!.ClaimAsync().ConfigureAwait(false);
        await ExecuteLockedAsync(
                InvalidateRuntimeGeneration,
                CancellationToken.None)
            .ConfigureAwait(false);
        await DisposeHandlerActivationAsync().ConfigureAwait(false);
    }

    private void CloseHandlerActivation()
    {
        lock (_sessionGate)
            _handlerActivationClosed = true;
    }

    public bool IsConfigured { get; private set; }

    public bool ContextInvalidated =>
        Volatile.Read(ref _contextInvalidated) != 0;

    internal void FenceRuntimeGeneration() =>
        Interlocked.Exchange(ref _contextInvalidated, 1);

    private volatile ZLinkActorDestroyPhase _destroyPhase;
    private volatile bool _teardownPending;
    private volatile bool _reservedCreationPending;
    private int _deferredJoinPending;

    public bool IsDispatchBlocked =>
        _destroyPhase != ZLinkActorDestroyPhase.None
        || _teardownPending
        || _reservedCreationPending;

    public bool IsTeardownPending => _teardownPending;

    public ZLinkActorDispatchMailbox.BarrierReservation?
        ReserveDeferredJoinBarrier(out Task? targetCompletion)
    {
        targetCompletion = null;
        if (IsDispatchBlocked
            || Interlocked.CompareExchange(ref _deferredJoinPending, 1, 0) != 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{ActorId}' already has an active lifecycle transition.");

        try
        {
            targetCompletion = Handoff.BeginDeferredJoinCapture();
            if (targetCompletion is not null)
                return null;
            return _dispatchMailbox.ReserveBarrier();
        }
        catch
        {
            _ = Handoff.EndDeferredJoinCapture();
            Volatile.Write(ref _deferredJoinPending, 0);
            throw;
        }
    }

    public ZLinkActorDispatchMailbox.BarrierReservation
        ReserveDeferredJoinBarrierAfterTarget()
    {
        try
        {
            return _dispatchMailbox.ReserveBarrier();
        }
        catch
        {
            _ = Handoff.EndDeferredJoinCapture();
            Volatile.Write(ref _deferredJoinPending, 0);
            throw;
        }
    }

    internal ZLinkActorDispatchMailbox.BarrierReservation
        ReserveHandoffRestoreBarrier() => _dispatchMailbox.ReserveBarrier();

    public void EnsureDeferredJoinIdentity(IZLinkActor actor, ulong objectGeneration)
    {
        if (ContextInvalidated
            || !ReferenceEquals(Actor, actor)
            || NativeActorRef is not { Generation: var currentGeneration }
            || currentGeneration != objectGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{ActorId}' no longer matches the context that registered the Join.");
    }

    public void ReleaseDeferredJoinBarrier()
    {
        Volatile.Write(ref _deferredJoinPending, 0);
    }

    public void BeginReservedCreation()
    {
        EnsureReusable();
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_reserved_begin actor={ActorId} "
            + $"actor_present={Actor is not null} "
            + $"native_generation={NativeActorRef?.Generation.ToString() ?? "<none>"} "
            + $"retired_generation={RetiredLocalActorRef?.Generation.ToString() ?? "<none>"} "
            + $"creation_task={_actorCreationTask is not null} "
            + $"context_invalidated={ContextInvalidated}");
        _reservedCreationPending = true;
    }

    public void PublishReservedCreation()
    {
        _reservedCreationPending = false;
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_reserved_published actor={ActorId} "
            + $"actor_present={Actor is not null} "
            + $"native_generation={NativeActorRef?.Generation.ToString() ?? "<none>"}");
    }

    public ZLinkSpotActivation? LiveActivation
        => Activation is { IsDisposed: false } activation ? activation : null;

    public string? SpotId => LiveActivation?.SpotId;

    public void AttachStream(IZLinkStream stream)
    {
        SessionId = stream.SessionId;
        Stream = stream;
    }

    public bool DetachStreamIfCurrent(IZLinkStream stream)
    {
        if (!string.Equals(SessionId, stream.SessionId, StringComparison.Ordinal)) return false;

        SessionId = null;
        Stream = null;
        return true;
    }

    public void JoinSpot(ZLinkSpotActivation activation)
    {
        Context?.UpdateSameNodeSpot(activation.SpotId);
        Activation = activation;
        EnsureActorMetric();
    }

    public void LeaveSpotIfCurrent(ZLinkSpotActivation activation)
    {
        if (!ReferenceEquals(Activation, activation)) return;

        Context?.UpdateSameNodeSpot(null);
        Activation = null;
    }

    public void BindNativeActorRef(ZLinkBackendActorRef actorRef)
    {
        EnsureReusable();
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_native_bound actor={ActorId} "
            + $"generation={actorRef.Generation} node={actorRef.NodeRid} "
            + $"previous_generation={NativeActorRef?.Generation.ToString() ?? "<none>"}");
        NativeActorRef = actorRef;
    }

    public bool BindActorInstance(IZLinkActor actor)
    {
        EnsureReusable();
        if (Context is { } expectedContext
            && !ReferenceEquals(actor.Context, expectedContext))
            throw new InvalidOperationException(
                $"Actor '{ActorId}' must expose its framework-issued Context.");

        if (Actor is not null
            && !ReferenceEquals(Actor, actor)
            && (SessionId is not null || Activation is not null))
            throw new InvalidOperationException(
                $"Actor id '{ActorId}' is already bound to another actor instance.");

        if (ReferenceEquals(Actor, actor)) return false;

        Actor = actor;
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_instance_bound actor={ActorId} "
            + $"native_generation={NativeActorRef?.Generation.ToString() ?? "<none>"} "
            + $"native_node={NativeActorRef?.NodeRid.ToString() ?? "<none>"}");
        EnsureActorMetric();
        IsConfigured = false;
        return true;
    }

    public ZLinkActorContext GetOrCreateContext(Func<ZLinkActorContext> createContext)
    {
        EnsureReusable();
        if (Context is not null) return Context;

        Interlocked.Exchange(ref _contextInvalidated, 0);
        Context = createContext();
        EnsureActorMetric();
        return Context;
    }

    public bool TryBeginActorConfiguration()
    {
        if (IsConfigured) return false;

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
        string meshName = "",
        ulong targetNodeGeneration = 1,
        ulong ownerLeaseGeneration = 0,
        ulong sessionOwnerNodeGeneration = 1,
        ulong acceptedHighWater = 0)
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
            acceptedHighWater);
        lock (_sessionGate)
        {
            PurgeExpiredSessionBindingTombstones();
            EnsureBindingTokenCanBeUsed(replacement);
            if (_sessionReplacement is not null)
                throw ReplacementInProgress();
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
        }
    }

    public ZLinkActorSessionReplacementAttempt BeginSessionReplacement(
        RoutingId? sessionNodeRid,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        string meshName,
        ulong targetNodeGeneration,
        ulong ownerLeaseGeneration,
        ulong sessionOwnerNodeGeneration,
        ulong acceptedHighWater,
        ZLinkActorPreviousBindingFence? previousFence = null)
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
            acceptedHighWater);
        lock (_sessionGate)
        {
            PurgeExpiredSessionBindingTombstones();
            EnsureBindingTokenCanBeUsed(replacement);
            if (_sessionReplacement is { } currentReplacement)
            {
                var pendingBinding = currentReplacement.Replacement;
                if (SameBindingToken(pendingBinding, replacement))
                {
                    EnsureExactBindingIdentity(pendingBinding, replacement);
                    if (currentReplacement.PreviousFence != previousFence)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.InvalidOperation,
                            $"Actor '{ActorId}' received conflicting previous binding fields for one replacement token.",
                            ZLinkRetryAdvice.DoNotRetry);
                    if (!currentReplacement.ExecutionActive)
                    {
                        currentReplacement.ExecutionActive = true;
                        currentReplacement.Completion =
                            new TaskCompletionSource<Exception?>(
                                TaskCreationOptions.RunContinuationsAsynchronously);
                        return currentReplacement.CreateAttempt(ownsExecution: true);
                    }
                    return currentReplacement.CreateAttempt(ownsExecution: false);
                }
                throw ReplacementInProgress();
            }
            if (_boundSession is { } current
                && SameBindingToken(current, replacement))
            {
                EnsureExactBindingIdentity(current, replacement);
                return new ZLinkActorSessionReplacementAttempt(
                    replacement,
                    Previous: null,
                    Task.FromResult<Exception?>(null),
                    OwnsExecution: false);
            }

            _sessionReplacement = new SessionBindingReplacement(
                replacement,
                _boundSession,
                previousFence);
            return _sessionReplacement.CreateAttempt(ownsExecution: true);
        }
    }

    public void MarkPreviousSessionBindingTombstoned(
        ZLinkActorSessionReplacementAttempt attempt)
    {
        if (!attempt.OwnsExecution)
            throw ReplacementWasInvalidated();
        lock (_sessionGate)
        {
            var replacement = GetCurrentReplacement(attempt);
            replacement.PreviousBindingTombstoned = true;
        }
    }

    public void PublishSessionReplacement(
        ZLinkActorSessionReplacementAttempt attempt)
    {
        if (!attempt.OwnsExecution)
            throw ReplacementWasInvalidated();
        lock (_sessionGate)
        {
            var replacement = GetCurrentReplacement(attempt);
            if (replacement.Phase >= ZLinkActorSessionReplacementPhase.Published)
                return;
            if (replacement.Previous is { } previous)
                RememberRetiredSessionBinding(previous);
            _boundSession = replacement.Replacement;
            replacement.Phase = ZLinkActorSessionReplacementPhase.Published;
        }
    }

    public void CompleteSessionReplacement(
        ZLinkActorSessionReplacementAttempt attempt)
    {
        if (!attempt.OwnsExecution)
            throw ReplacementWasInvalidated();
        TaskCompletionSource<Exception?> completion;
        lock (_sessionGate)
        {
            var replacement = GetCurrentReplacement(attempt);
            if (replacement.Phase != ZLinkActorSessionReplacementPhase.Published)
                throw ReplacementWasInvalidated();
            replacement.Phase = ZLinkActorSessionReplacementPhase.Completed;
            replacement.ExecutionActive = false;
            completion = replacement.Completion;
            _sessionReplacement = null;
        }
        completion.TrySetResult(null);
    }

    public void AbortSessionReplacement(
        ZLinkActorSessionReplacementAttempt attempt,
        Exception failure)
    {
        ArgumentNullException.ThrowIfNull(failure);
        if (!attempt.OwnsExecution) return;
        TaskCompletionSource<Exception?> completion;
        lock (_sessionGate)
        {
            if (!IsCurrentReplacement(attempt.Replacement)
                || _sessionReplacement is not { ExecutionActive: true } replacement)
                return;
            completion = replacement.Completion;
            replacement.ExecutionActive = false;
            // Before the first durable side effect, rollback is safe. Once the
            // previous binding is tombstoned or publication occurs, retain the
            // aggregate so an exact retry can only move forward.
            if (replacement.Phase == ZLinkActorSessionReplacementPhase.Prepared
                && !replacement.PreviousBindingTombstoned)
                _sessionReplacement = null;
        }
        completion.TrySetResult(failure);
    }

    internal int SessionBindingTombstoneCount
    {
        get
        {
            lock (_sessionGate)
            {
                PurgeExpiredSessionBindingTombstones();
                return _sessionBindingTombstones.Count;
            }
        }
    }

    private ZLinkActorBoundSession CreateSessionBinding(
        RoutingId? sessionNodeRid,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        string meshName,
        ulong targetNodeGeneration,
        ulong ownerLeaseGeneration,
        ulong sessionOwnerNodeGeneration,
        ulong acceptedHighWater)
    {
        EnsureReusable();
        if (bindingToken.Length == 0)
            throw new InvalidOperationException(
                "Actor session binding token must not be empty.");
        if (sessionRid.IsEmpty
            || bindingGeneration == 0
            || objectGeneration == 0
            || authorityOwnerGeneration == 0
            || string.IsNullOrWhiteSpace(meshName)
            || targetNodeGeneration == 0
            || ownerLeaseGeneration == 0
            || sessionOwnerNodeGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{ActorId}' session binding requires an exact Actor, Session, Mesh, node lifecycle, authority, and owner lease.");

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
            acceptedHighWater);
    }

    private void EnsureBindingTokenCanBeUsed(
        ZLinkActorBoundSession replacement)
    {
        if (!_sessionBindingTombstones.TryGetValue(
                replacement.BindingToken,
                out var tombstone))
            return;
        EnsureExactBindingIdentity(tombstone.Binding, replacement);
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"Actor '{ActorId}' session binding operation was already replaced.",
            ZLinkRetryAdvice.DoNotRetry);
    }

    private static bool SameBindingToken(
        ZLinkActorBoundSession left,
        ZLinkActorBoundSession right) =>
        string.Equals(
            left.BindingToken,
            right.BindingToken,
            StringComparison.Ordinal);

    private void EnsureExactBindingIdentity(
        ZLinkActorBoundSession expected,
        ZLinkActorBoundSession actual)
    {
        if (expected == actual) return;
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"Actor '{ActorId}' received conflicting fields for one session binding token.",
            ZLinkRetryAdvice.DoNotRetry);
    }

    private bool IsCurrentReplacement(
        ZLinkActorBoundSession replacement) =>
        _sessionReplacement is { } current
        && current.Replacement == replacement;

    private SessionBindingReplacement GetCurrentReplacement(
        ZLinkActorSessionReplacementAttempt attempt)
    {
        if (_sessionReplacement is not { ExecutionActive: true } replacement
            || replacement.Replacement != attempt.Replacement)
            throw ReplacementWasInvalidated();
        return replacement;
    }

    private ZLinkFrameworkException ReplacementInProgress() =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Actor '{ActorId}' is completing another session binding replacement.",
            ZLinkRetryAdvice.RetryAfterBackoff);

    private ZLinkFrameworkException ReplacementWasInvalidated() =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Actor '{ActorId}' session binding replacement lost its exact local authority before publication.",
            ZLinkRetryAdvice.RetryAfterBackoff);

    private void RememberRetiredSessionBinding(
        ZLinkActorBoundSession binding)
    {
        PurgeExpiredSessionBindingTombstones();
        if (!_sessionBindingTombstones.ContainsKey(binding.BindingToken)
            && _sessionBindingTombstones.Count >= _maxSessionBindingTombstones)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{ActorId}' session binding tombstone capacity is exhausted.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        _sessionBindingTombstones[binding.BindingToken] =
            new ZLinkActorSessionBindingTombstone(
                binding,
                _timeProvider.GetUtcNow()
                + _sessionBindingTombstoneRetention);
    }

    private void PurgeExpiredSessionBindingTombstones()
    {
        if (_sessionBindingTombstones.Count == 0) return;
        var now = _timeProvider.GetUtcNow();
        foreach (var token in _sessionBindingTombstones
                     .Where(entry => entry.Value.ExpiresAt <= now)
                     .Select(static entry => entry.Key)
                     .ToArray())
            _sessionBindingTombstones.Remove(token);
    }

    public void RecordBoundSessionAccepted(string bindingToken)
    {
        lock (_sessionGate)
        {
            if (_boundSession is not { } current
                || !string.Equals(
                    current.BindingToken,
                    bindingToken,
                    StringComparison.Ordinal))
                return;
            _boundSession = current with
            {
                AcceptedHighWater = checked(current.AcceptedHighWater + 1)
            };
        }
    }

    public void RecordRelocatedSessionAccepted(RoutingId sessionRid)
    {
        lock (_sessionGate)
        {
            if (_pendingSessionRoute is { } pending
                && pending.Route.SessionRid is { } pendingSessionRid
                && pendingSessionRid == sessionRid)
            {
                _pendingSessionRoute = pending with
                {
                    Route = pending.Route with
                    {
                        AcceptedHighWater =
                            checked(pending.Route.AcceptedHighWater + 1)
                    }
                };
                return;
            }

            if (_boundSession is not { } current
                || current.SessionRid != sessionRid)
                return;
            _boundSession = current with
            {
                AcceptedHighWater = checked(current.AcceptedHighWater + 1)
            };
        }
    }

    // A relayed frame carries the sequence assigned by the Session owner.
    // That value is authoritative across relay hops; incrementing it at
    // every hop counts one accepted frame more than once.
    public void RecordRelocatedSessionAccepted(
        RoutingId sessionRid,
        ulong acceptedHighWater)
    {
        if (acceptedHighWater == 0) return;

        lock (_sessionGate)
        {
            if (_pendingSessionRoute is { } pending
                && pending.Route.SessionRid is { } pendingSessionRid
                && pendingSessionRid == sessionRid)
            {
                if (acceptedHighWater
                    <= pending.Route.AcceptedHighWater)
                    return;
                _pendingSessionRoute = pending with
                {
                    Route = pending.Route with
                    {
                        AcceptedHighWater = acceptedHighWater
                    }
                };
                return;
            }

            if (_boundSession is not { } current
                || current.SessionRid != sessionRid
                || acceptedHighWater <= current.AcceptedHighWater)
                return;
            _boundSession = current with
            {
                AcceptedHighWater = acceptedHighWater
            };
        }
    }

    public void StageRelocationSessionRoute(
        string handoffId,
        ZLinkRemoteActorBoundSessionRoute route)
    {
        lock (_sessionGate)
        {
            if (!route.IsBound)
            {
                _pendingSessionRoute = null;
                return;
            }
            if (_pendingSessionRoute is { } pending
                && !string.Equals(
                    pending.HandoffId,
                    handoffId,
                    StringComparison.Ordinal))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{ActorId}' already stages another session route.");
            _pendingSessionRoute = new ZLinkPendingActorSessionRoute(
                handoffId,
                route,
                TargetActor: null,
                TargetAuthorityOwnerGeneration: 0);
        }
    }

    public void MarkRelocationSessionAuthorityCommitted(
        string handoffId,
        ZLinkBackendActorRef targetActor,
        ulong targetAuthorityOwnerGeneration,
        string targetMeshName,
        ulong targetNodeGeneration,
        ulong targetOwnerLeaseGeneration)
    {
        lock (_sessionGate)
        {
            if (_pendingSessionRoute is not { } pending)
                return;
            if (!string.Equals(pending.HandoffId, handoffId, StringComparison.Ordinal)
                || pending.Route.ObjectGeneration != targetActor.Generation
                || string.IsNullOrWhiteSpace(targetMeshName)
                || targetNodeGeneration == 0
                || targetOwnerLeaseGeneration == 0
                || targetAuthorityOwnerGeneration
                <= pending.Route.AuthorityOwnerGeneration)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{ActorId}' staged session route does not match committed authority.");
            _pendingSessionRoute = pending with
            {
                TargetActor = targetActor,
                TargetAuthorityOwnerGeneration = targetAuthorityOwnerGeneration,
                TargetMeshName = targetMeshName,
                TargetNodeGeneration = targetNodeGeneration,
                TargetOwnerLeaseGeneration = targetOwnerLeaseGeneration
            };
        }
    }

    public bool TryGetCommittedRelocationSessionRoute(
        string handoffId,
        out ZLinkPendingActorSessionRoute route)
    {
        lock (_sessionGate)
        {
            if (_pendingSessionRoute is
                {
                    TargetActor: not null,
                    TargetAuthorityOwnerGeneration: > 0
                } pending
                && string.Equals(
                    pending.HandoffId,
                    handoffId,
                    StringComparison.Ordinal))
            {
                route = pending;
                return true;
            }
        }

        route = default;
        return false;
    }

    public bool TryGetStagedRelocationSessionRoute(
        string handoffId,
        out ZLinkRemoteActorBoundSessionRoute route)
    {
        lock (_sessionGate)
        {
            if (_pendingSessionRoute is { } pending
                && string.Equals(
                    pending.HandoffId,
                    handoffId,
                    StringComparison.Ordinal))
            {
                route = pending.Route;
                return true;
            }
        }
        route = default;
        return false;
    }

    public void CompleteRelocationSessionRoute(string handoffId)
    {
        lock (_sessionGate)
        {
            if (_pendingSessionRoute is not { } pending
                || !string.Equals(
                    pending.HandoffId,
                    handoffId,
                    StringComparison.Ordinal))
                return;
            _boundSession = CreateCommittedRelocationSession(pending);
            _pendingSessionRoute = null;
        }
    }

    public void AbortRelocationSessionRoute(string handoffId)
    {
        lock (_sessionGate)
        {
            if (_pendingSessionRoute is { } pending
                && string.Equals(
                    pending.HandoffId,
                    handoffId,
                    StringComparison.Ordinal))
                _pendingSessionRoute = null;
        }
    }

    public void UnbindSession(string bindingToken)
    {
        if (bindingToken.Length == 0) return;

        TaskCompletionSource<Exception?>? completion = null;
        lock (_sessionGate)
        {
            if (_sessionReplacement is { Replacement.BindingToken: var pending }
                && string.Equals(pending, bindingToken, StringComparison.Ordinal))
            {
                completion = _sessionReplacement.Completion;
                _sessionReplacement = null;
            }
            else if (_boundSession is { BindingToken: var current }
                     && string.Equals(
                         current,
                         bindingToken,
                         StringComparison.Ordinal))
            {
                _boundSession = null;
                if (_sessionReplacement is not null)
                    _sessionReplacement = null;
            }
            else if (_pendingSessionRoute is
                         { Route.BindingToken: var pendingToken }
                     && string.Equals(pendingToken, bindingToken,
                         StringComparison.Ordinal))
            {
                // A physical disconnect can be replayed while the target
                // route is staged but before the session-owner commit. The
                // exact token invalidates that pending route as well; keeping
                // it would make completion retry a binding the session owner
                // has already removed.
                _pendingSessionRoute = null;
            }
        }
        completion?.TrySetResult(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{ActorId}' session binding replacement was unbound before commit.",
                ZLinkRetryAdvice.RetryAfterBackoff));
    }

    public void TombstoneSession(ZLinkActorBoundSession expected)
    {
        lock (_sessionGate)
        {
            PurgeExpiredSessionBindingTombstones();
            if (_sessionBindingTombstones.TryGetValue(
                    expected.BindingToken,
                    out var tombstone))
            {
                EnsureExactBindingIdentity(tombstone.Binding, expected);
                return;
            }
            if (_boundSession is not { } current
                || !SameBindingToken(current, expected))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{ActorId}' no longer has the exact session binding selected for replacement.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            EnsureExactBindingIdentity(current, expected);
            RememberRetiredSessionBinding(current);
            _boundSession = null;
        }
    }

    public bool TryGetBoundSession(out ZLinkActorBoundSession session)
    {
        lock (_sessionGate)
        {
            if (_boundSession is { } current)
            {
                session = current;
                return true;
            }
        }

        session = default;
        return false;
    }

    internal bool TryGetBoundSessionForOutbound(
        out ZLinkActorBoundSession session)
    {
        lock (_sessionGate)
        {
            // Once the target authority is committed, a relocation can still
            // retain the source projection in _boundSession until the session
            // owner acknowledges the route switch. Outbound pushes must use
            // the committed target projection during that interval; otherwise
            // the first target push is fenced as a stale source push.
            if (_pendingSessionRoute is
                {
                    TargetActor: not null,
                    TargetAuthorityOwnerGeneration: > 0
                } pending)
            {
                session = CreateCommittedRelocationSession(pending);
                return true;
            }
            if (_boundSession is { } current)
            {
                session = current;
                return true;
            }
        }

        session = default;
        return false;
    }

    internal bool TryGetBoundSessionForInbound(
        out ZLinkActorBoundSession session)
    {
        lock (_sessionGate)
        {
            // During relocation, the target accepts frames before the staged
            // route is promoted to _boundSession. Use the committed target
            // projection while retaining the source session identity fence.
            if (_pendingSessionRoute is
                {
                    TargetActor: not null,
                    TargetAuthorityOwnerGeneration: > 0
                } pending)
            {
                session = CreateCommittedRelocationSession(pending);
                return true;
            }
            if (_boundSession is { } current)
            {
                session = current;
                return true;
            }
        }

        session = default;
        return false;
    }

    private static ZLinkActorBoundSession CreateCommittedRelocationSession(
        ZLinkPendingActorSessionRoute pending)
    {
        var target = pending.TargetActor
                     ?? throw new InvalidOperationException(
                         "A session route cannot complete before authority commit.");
        var route = pending.Route;
        return new ZLinkActorBoundSession(
            route.NodeRid,
            route.SessionRid!.Value,
            route.BindingToken!,
            route.BindingGeneration,
            target.Generation,
            pending.TargetAuthorityOwnerGeneration,
            pending.TargetMeshName!,
            pending.TargetNodeGeneration,
            pending.TargetOwnerLeaseGeneration,
            route.SessionOwnerNodeGeneration,
            route.AcceptedHighWater);
    }

    public bool TryUseBoundSession(
        string expectedBindingToken,
        Func<ZLinkActorBoundSession, bool> operation)
    {
        ArgumentNullException.ThrowIfNull(operation);

        lock (_sessionGate)
        {
            if (_boundSession is not { } current
                || !string.Equals(current.BindingToken, expectedBindingToken, StringComparison.Ordinal))
                return true;

            return operation(current);
        }
    }

    public ZLinkActorBoundSession? ClearAfterDestroy()
    {
        return TransitionLocalInstance(ZLinkActorTerminalTransition.Destroyed, null);
    }

    public void InvalidateRuntimeGeneration()
    {
        var failure = new InvalidOperationException(
            $"Actor '{ActorId}' belongs to a stopped framework runtime generation.");
        var teardownAttempt = _teardownAttempt;
        Handoff.AbortRuntimeGeneration(failure);
        ClearAfterDestroy();
        teardownAttempt?.TrySetResult(failure);
    }

    public void RetireMigratedActorInstance(ZLinkBackendActorRef sourceActor)
    {
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_migrated actor={ActorId} "
            + $"source_generation={sourceActor.Generation} "
            + $"current_generation={NativeActorRef?.Generation.ToString() ?? "<none>"}");
        _ = TransitionLocalInstance(
            ZLinkActorTerminalTransition.Migrated,
            sourceActor);
    }

    private ZLinkActorBoundSession? TransitionLocalInstance(
        ZLinkActorTerminalTransition transition,
        ZLinkBackendActorRef? retiredLocalActor)
    {
        CloseHandlerActivation();
        ZLinkActorBoundSession? releasedBoundSession = null;
        TaskCompletionSource<Exception?>? replacementCompletion = null;
        if (transition is ZLinkActorTerminalTransition.Destroyed
            or ZLinkActorTerminalTransition.Migrated)
            lock (_sessionGate)
            {
                if (transition == ZLinkActorTerminalTransition.Destroyed)
                {
                    releasedBoundSession = _boundSession;
                    _boundSession = null;
                }
                replacementCompletion = _sessionReplacement?.Completion;
                _sessionReplacement = null;
                if (transition == ZLinkActorTerminalTransition.Destroyed)
                    _sessionBindingTombstones.Clear();
            }
        replacementCompletion?.TrySetResult(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{ActorId}' changed local authority during session binding replacement.",
                ZLinkRetryAdvice.RetryAfterBackoff));

        SessionId = null;
        Stream = null;
        Activation = null;
        CurrentDispatch = null;
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
                _teardownPending = false;
                _teardownAttempt = null;
                Handoff.Reset();
                _pendingSessionRoute = null;
                break;
            case ZLinkActorTerminalTransition.Migrated:
                RetiredLocalActorRef = retiredLocalActor
                    ?? throw new ArgumentNullException(nameof(retiredLocalActor));
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
                Interlocked.Exchange(ref _actorMetricMeshName, null)!);
    }

    private void EnsureActorMetric()
    {
        if (Actor is null || Volatile.Read(ref _actorMetricActive) != 0) return;
        var meshName = Activation?.MeshName ?? Context?.MeshName;
        if (string.IsNullOrWhiteSpace(meshName)) return;
        if (Interlocked.CompareExchange(ref _actorMetricActive, 1, 0) != 0) return;
        _actorMetricMeshName = meshName;
        ZLinkRuntimeMetrics.RecordActorCreated(meshName);
    }

    private enum ZLinkActorTerminalTransition
    {
        Destroyed,
        Migrated
    }

    private sealed class EmptyServiceProvider : IServiceProvider
    {
        public object? GetService(Type serviceType) => null;
    }

    public void PrepareForTransferredActivation()
    {
        if (Actor is not null || IsConfigured)
            throw new InvalidOperationException(
                $"Actor '{ActorId}' already has an active local instance.");

        Handoff.PrepareForTransferredActivation();
        NativeActorRef = null;
        Interlocked.Exchange(ref _contextInvalidated, 0);
        lock (_terminalLifecycleGate)
        {
            if (_terminalLifecycleCompletion is { IsCompleted: false })
                throw new InvalidOperationException(
                    $"Actor '{ActorId}' still has a pending terminal lifecycle completion.");

            _dispatchMailbox.ReopenAdmission();
            _terminalLifecycleCompletion = null;
            lock (_sessionGate)
                _handlerActivationClosed = false;
        }
    }

    public void ClearRetiredLocalActorRef(ZLinkBackendActorRef actor)
    {
        if (RetiredLocalActorRef == actor) RetiredLocalActorRef = null;
    }

    public void BeginTeardown()
    {
        CloseHandlerActivation();
        _teardownPending = true;
        FenceRuntimeGeneration();
    }

    public ZLinkActorTeardownOperation BeginOrJoinTeardownAttempt()
    {
        if (!_teardownPending)
            throw new InvalidOperationException(
                $"Actor '{ActorId}' does not have a pending teardown.");
        if (_teardownAttempt is { } existing)
            return new ZLinkActorTeardownOperation(existing.Task, false, false);

        var nativeAlreadyDestroyed = false;
        switch (_destroyPhase)
        {
            case ZLinkActorDestroyPhase.None:
                _destroyPhase = ZLinkActorDestroyPhase.DestroyingNative;
                break;
            case ZLinkActorDestroyPhase.NativeDestroyed:
                _destroyPhase = ZLinkActorDestroyPhase.ReleasingOwnership;
                nativeAlreadyDestroyed = true;
                break;
            default:
                throw new InvalidOperationException(
                    $"Actor '{ActorId}' teardown phase has no owning operation.");
        }

        _teardownAttempt = new TaskCompletionSource<Exception?>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        return new ZLinkActorTeardownOperation(
            _teardownAttempt.Task,
            true,
            nativeAlreadyDestroyed);
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
        Exception failure)
    {
        EnsureCurrentTeardownAttempt(operation);
        if (nativeDestroyed)
            _destroyPhase = ZLinkActorDestroyPhase.NativeDestroyed;
        else
            _destroyPhase = ZLinkActorDestroyPhase.None;
        var completion = _teardownAttempt!;
        _teardownAttempt = null;
        completion.TrySetResult(failure);
    }

    public ZLinkActorBoundSession? CompleteTeardownAttempt(ZLinkActorTeardownOperation operation)
    {
        EnsureCurrentTeardownAttempt(operation);
        var completion = _teardownAttempt!;
        var boundSession = ClearAfterDestroy();
        completion.TrySetResult(null);
        return boundSession;
    }

    private void EnsureCurrentTeardownAttempt(ZLinkActorTeardownOperation operation)
    {
        if (_teardownAttempt is null || !ReferenceEquals(_teardownAttempt.Task, operation.Completion))
            throw new InvalidOperationException(
                $"Actor '{ActorId}' teardown operation is no longer current.");
    }

    public void EnsureContextValid()
    {
        if (ContextInvalidated)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor context for '{ActorId}' cannot start an operation after owner cutover.");
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
        CancellationToken cancellationToken)
    {
        var created = false;
        var task = await ExecuteLockedAsync(
            () =>
            {
                EnsureReusable();
                if (ActorType is not null
                    && !string.Equals(ActorType, actorType, StringComparison.Ordinal))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.TypeMismatch,
                        $"Actor '{ActorId}' already uses actor type '{ActorType}', not '{actorType}'.");

                if (Actor is not null && ContextInvalidated)
                {
                    Actor = null;
                    Context = null;
                    NativeActorRef = null;
                    IsConfigured = false;
                    Interlocked.Exchange(ref _contextInvalidated, 0);
                }
                else if (Actor is null
                         && ContextInvalidated
                         && _actorCreationTask is null)
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
                            $"Actor '{ActorId}' already exists.");

                    return Task.FromResult(Actor);
                }

                if (_actorCreationTask is null)
                {
                    ActorType = actorType;
                    created = true;
                    _actorCreationTask = createActor();
                    _ = ClearActorCreationTaskWhenCompletedAsync(_actorCreationTask);
                }
                else if (failIfExists)
                {
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.AlreadyExists,
                        $"Actor '{ActorId}' is already being created.");
                }

                return _actorCreationTask;
            },
            cancellationToken).ConfigureAwait(false);

        return new ZLinkActorCreationOperation(task, created);
    }

    public async ValueTask ExecuteLockedAsync(
        Action operation,
        CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            operation();
        }
        finally
        {
            _gate.Release();
        }
    }

    public async ValueTask<T> ExecuteLockedAsync<T>(
        Func<T> operation,
        CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return operation();
        }
        finally
        {
            _gate.Release();
        }
    }

    public async ValueTask<T> ExecuteLockedAsync<T>(
        Func<CancellationToken, ValueTask<T>> operation,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(operation);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return await operation(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    public async ValueTask<T> ExecuteHandoffTransitionAsync<T>(
        Func<T> transition,
        CancellationToken cancellationToken)
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
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<int> BeginHandoffCaptureAsync(CancellationToken cancellationToken)
    {
        if (AmbientDispatch.Value is { IsActive: true } ownership
            && ReferenceEquals(ownership.State, this))
        {
            return await ExecuteLockedAsync(
                    () =>
                    {
                        EnsureReusable();
                        var pendingRequests = _dispatchMailbox.PendingRequestCount;
                        Handoff.BeginCapture();
                        return pendingRequests;
                    },
                    cancellationToken)
                .ConfigureAwait(false);
        }

        using var turn = await _dispatchMailbox.EnterAsync(cancellationToken).ConfigureAwait(false);
        return await ExecuteLockedAsync(
                () =>
                {
                    EnsureReusable();
                    var pendingRequests = _dispatchMailbox.PendingRequestCount;
                    Handoff.BeginCapture();
                    return pendingRequests;
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask ExecuteDispatchAsync(
        ZlinkStreamHeader header,
        Func<CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        using var turn = await _dispatchMailbox.EnterAsync(
                cancellationToken,
                countAsPendingMessage: true)
            .ConfigureAwait(false);
        EnsureDispatchAvailable();
        using var dispatch = EnterDispatch(header);
        await operation(cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<T> ExecuteDispatchAsync<T>(
        ZlinkStreamHeader header,
        Func<CancellationToken, ValueTask<T>> operation,
        bool countAsPendingRequest,
        CancellationToken cancellationToken,
        bool allowRelocationReplay = false)
    {
        using var turn = await _dispatchMailbox.EnterAsync(
                cancellationToken,
                countAsPendingMessage: true,
                countAsPendingRequest)
            .ConfigureAwait(false);
        EnsureDispatchAvailable(allowRelocationReplay);
        using var dispatch = EnterDispatch(header);
        return await operation(cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask ExecuteLifecycleAsync(
        Func<CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        using var turn = await _dispatchMailbox.EnterAsync(cancellationToken).ConfigureAwait(false);
        EnsureDispatchAvailable();
        await operation(cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask ExecuteRelocationCompletionAsync(
        ulong objectGeneration,
        Func<CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        using var turn = await _dispatchMailbox.EnterAsync(cancellationToken)
            .ConfigureAwait(false);
        if (ContextInvalidated
            || NativeActorRef is not { Generation: var currentGeneration }
            || currentGeneration != objectGeneration
            || Actor is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{ActorId}' no longer matches its durable Join completion.");
        await operation(cancellationToken).ConfigureAwait(false);
    }

    private void EnsureDispatchAvailable(bool allowRelocationReplay = false)
    {
        if (!IsDispatchBlocked
            && (allowRelocationReplay || !Handoff.BlocksLocalDispatch))
            return;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"Actor '{ActorId}' is not available while its lifecycle transition is active.");
    }

    private void EnsureReusable()
    {
        // Reserved creation deliberately blocks public dispatch until the
        // authority Ready commit, but the factory, context and native Actor
        // binding that prepare that reservation must still mutate this state.
        if (_destroyPhase == ZLinkActorDestroyPhase.None && !_teardownPending)
            return;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"Actor '{ActorId}' cannot be reused while teardown is incomplete.");
    }

    public DispatchScope EnterDispatch(ZlinkStreamHeader header)
    {
        var previous = CurrentDispatch;
        var previousAmbient = AmbientDispatch.Value;
        var ownership = new DispatchOwnership(this);
        CurrentDispatch = new ZLinkActorDispatchState(header);
        AmbientDispatch.Value = ownership;
        return new DispatchScope(this, previous, previousAmbient, ownership);
    }

    public DispatchScope EnterDeferredJoinExecution()
    {
        var previousAmbient = AmbientDispatch.Value;
        var ownership = new DispatchOwnership(this);
        AmbientDispatch.Value = ownership;
        return new DispatchScope(
            this,
            CurrentDispatch,
            previousAmbient,
            ownership);
    }

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
            CancellationToken.None).ConfigureAwait(false);
    }

    private void ClearFailedActorCreationLocked()
    {
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_creation_failed_clear actor={ActorId} "
            + $"native_generation={NativeActorRef?.Generation.ToString() ?? "<none>"} "
            + $"actor_present={Actor is not null} "
            + $"teardown={_teardownPending}");
        _actorCreationTask = null;
        if (_teardownPending) return;

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
        private readonly ZLinkActorRuntimeState? _state;
        private readonly ZLinkActorDispatchState? _previous;
        private readonly DispatchOwnership? _previousAmbient;
        private readonly DispatchOwnership? _ownership;

        internal DispatchScope(
            ZLinkActorRuntimeState state,
            ZLinkActorDispatchState? previous,
            DispatchOwnership? previousAmbient,
            DispatchOwnership ownership)
        {
            _state = state;
            _previous = previous;
            _previousAmbient = previousAmbient;
            _ownership = ownership;
        }

        public void Dispose()
        {
            if (_state is not null)
            {
                _ownership?.Deactivate();
                _state.CurrentDispatch = _previous;
                AmbientDispatch.Value = _previousAmbient;
            }
        }
    }

    private sealed class SessionBindingReplacement(
        ZLinkActorBoundSession replacement,
        ZLinkActorBoundSession? previous,
        ZLinkActorPreviousBindingFence? previousFence)
    {
        public ZLinkActorBoundSession Replacement { get; } = replacement;

        public ZLinkActorBoundSession? Previous { get; } = previous;

        public ZLinkActorPreviousBindingFence? PreviousFence { get; } =
            previousFence;

        public TaskCompletionSource<Exception?> Completion { get; set; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ZLinkActorSessionReplacementPhase Phase { get; set; } =
            ZLinkActorSessionReplacementPhase.Prepared;

        public bool PreviousBindingTombstoned { get; set; }

        public bool ExecutionActive { get; set; } = true;

        public ZLinkActorSessionReplacementAttempt CreateAttempt(
            bool ownsExecution) =>
            new(
                Replacement,
                Previous,
                Completion.Task,
                ownsExecution,
                PreviousBindingTombstoned);
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
    ReleasingOwnership
}

internal enum ZLinkActorSessionReplacementPhase
{
    Prepared,
    Published,
    Completed
}

internal readonly record struct ZLinkActorTeardownOperation(
    Task<Exception?> Completion,
    bool OwnsExecution,
    bool NativeAlreadyDestroyed);

internal readonly record struct ZLinkActorHandlerTerminalCompletion<T>(
    Task<T> Completion,
    bool RequiresDispatchRelease);

// Actor-side delivery projection reconstructed from binding and relocation
// payloads. It does not publish or replace the Session owner's binding route.
internal readonly record struct ZLinkActorBoundSession(
    RoutingId? SessionNodeRid,
    RoutingId SessionRid,
    string BindingToken,
    ulong BindingGeneration = 1,
    ulong ObjectGeneration = 0,
    ulong AuthorityOwnerGeneration = 0,
    string MeshName = "",
    ulong TargetNodeGeneration = 1,
    ulong OwnerLeaseGeneration = 0,
    ulong SessionOwnerNodeGeneration = 1,
    ulong AcceptedHighWater = 0);

internal readonly record struct ZLinkActorSessionReplacementAttempt(
    ZLinkActorBoundSession Replacement,
    ZLinkActorBoundSession? Previous,
    Task<Exception?> Completion,
    bool OwnsExecution,
    bool PreviousBindingTombstoned = false);

internal readonly record struct ZLinkActorPreviousBindingFence(
    RoutingId TargetNodeRid,
    RoutingId SessionNodeRid,
    RoutingId SessionRid,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    string MeshName,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater);

internal readonly record struct ZLinkActorSessionBindingTombstone(
    ZLinkActorBoundSession Binding,
    DateTimeOffset ExpiresAt);

internal readonly record struct ZLinkPendingActorSessionRoute(
    string HandoffId,
    ZLinkRemoteActorBoundSessionRoute Route,
    ZLinkBackendActorRef? TargetActor,
    ulong TargetAuthorityOwnerGeneration,
    string? TargetMeshName = null,
    ulong TargetNodeGeneration = 0,
    ulong TargetOwnerLeaseGeneration = 0);

internal readonly record struct ZLinkActorCreationOperation(
    Task<IZLinkActor> Task,
    bool Created);

internal readonly record struct ZLinkActorPlacementSelection(
    ZLinkSpotActivation? Activation,
    bool Prune);
