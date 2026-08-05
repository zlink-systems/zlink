using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkSpotRelocationPhase
{
    PerActorShells,
    Aggregates
}

internal sealed class ZLinkSpotNodeCatalog(
    IServiceProvider services,
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration frameworkRegistration,
    ZLinkSpotNodeRegistration registration,
    IZLinkBackendSpotNode node,
    string spotChannelName,
    ZLinkCompletionAdmissionOwner completionAdmission,
    ZLinkLocationLifecycle? lifecycle,
    ZLinkTimerScheduler timerScheduler,
    ZLinkActivationConcurrencyAdmission? activationAdmission = null) : IAsyncDisposable
{
    // Idle eviction is maintenance work. Limit the amount of candidate
    // inspection in one tick so a large catalog cannot monopolize the
    // scheduler or delay application dispatch on the same node.
    internal const int IdleEvictionBatchSize = 64;

    private readonly object _disposeGate = new();
    private readonly CancellationTokenSource _idleEvictionStop = new();
    private readonly TimeSpan _instanceSpotIdleTimeout =
        registration.InstanceSpotIdleTimeout;
    private Task? _disposeTask;
    private Task? _idleEvictionTask;
    private string? _idleEvictionCursor;
    private int _idleEvictionStopped;
    private readonly ZLinkSpotActivationFactory _activationFactory = new(
        services,
        runtime,
        frameworkRegistration,
        registration,
        node,
        spotChannelName,
        completionAdmission,
        timerScheduler);
    private readonly ZLinkActivationConcurrencyAdmission _activationAdmission =
        activationAdmission ?? new(registration.MaxPendingActivations);
    private readonly ZLinkSpotRetireScheduler? _retireScheduler =
        CreateRetireScheduler(
            services,
            runtime,
            frameworkRegistration);

    private readonly object _gate = new();
    private readonly Dictionary<string, TaskCompletionSource<bool>> _closing = [];
    private readonly Dictionary<string, string> _instanceSpotTypes =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, Type> _preparedSpotTypes =
        new(StringComparer.Ordinal);
    private readonly Dictionary<Type, int> _generatedSpotCreations = [];
    private readonly Dictionary<string, PendingSpotCreation> _pending = [];
    private readonly Dictionary<string, ZLinkSpotActivation> _spots = [];
    private TaskCompletionSource? _creationsDrained;
    private int _activeCreations;
    private bool _closed;

    public IReadOnlyCollection<ZLinkSpotActivation> Spots => SnapshotActivations();

    internal void StartIdleEviction()
    {
        if (_instanceSpotIdleTimeout <= TimeSpan.Zero) return;
        lock (_disposeGate)
        {
            if (_idleEvictionTask is null)
                _idleEvictionTask = RunIdleEvictionAsync();
        }
    }

    internal ZLinkInstanceSpotCatalogSnapshot InstanceSpotSnapshot(
        string stableType)
    {
        lock (_gate)
        {
            var active = _instanceSpotTypes.Count(entry =>
                StringComparer.Ordinal.Equals(entry.Value, stableType)
                && _spots.ContainsKey(entry.Key));
            var activating = _pending.Values.Count(pending =>
                IsStableTypeLocked(pending.SpotType, stableType));
            activating += _preparedSpotTypes.Values.Count(spotType =>
                IsStableTypeLocked(spotType, stableType));
            activating += _generatedSpotCreations
                .Where(entry => IsStableTypeLocked(entry.Key, stableType))
                .Sum(static entry => entry.Value);
            var closing = _closing.Keys.Count(spotId =>
                _instanceSpotTypes.TryGetValue(
                    spotId,
                    out var currentType)
                && StringComparer.Ordinal.Equals(
                    currentType,
                    stableType));
            return new ZLinkInstanceSpotCatalogSnapshot(
                checked((ulong)active),
                checked((ulong)activating),
                checked((ulong)closing));
        }
    }

    internal async ValueTask<bool> TryDrainAsync(
        bool hostShutdown,
        CancellationToken cancellationToken)
    {
        var activations = SnapshotActivations();
        foreach (var activation in activations)
        {
            //  Close의 반환값을 버리면 "닫으라고 했는데 안 닫혔다"를 구분할 수 없다.
            //  Spec 28 §178은 Shutdown이 "이미 수락한 work와 resource를 정리한다"고
            //  정하고, config-11 OBS-C4는 "Spot에 closing callback을 알린 뒤"를
            //  요구한다. Member actor가 남았다고 close를 거부하면 그 callback조차
            //  불리지 않고 drain 루프가 deadline만 태운다.
            var closed = await CloseCoreAsync(
                    activation.SpotId,
                    null,
                    releaseLocation: true,
                    //  Shutdown만 member를 가진 채 닫는다. Relocate 배수는 actor를
                    //  옮긴 뒤 닫아야 하므로 가드를 유지해야 한다.
                    requireNoActors: !hostShutdown,
                    hostShutdown
                        ? ZLinkSpotCloseReason.HostShutdown
                        : ZLinkSpotCloseReason.ExplicitClose,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!closed)
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"spot_close_refused spot={activation.SpotId}");
        }

        lock (_gate)
        {
            //  Drain 루프는 이 bool만 보고 다시 돈다. 안 닫히는 spot이 무엇인지
            //  남기지 않으면 deadline 소진의 이유를 밖에서 알 수 없다.
            if (_spots.Count != 0)
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"spot_drain_pending count={_spots.Count} "
                    + $"spots={string.Join(",", _spots.Keys)}");
            return _spots.Count == 0;
        }
    }

    internal ValueTask<ZLinkFrameworkRelocationReason?> PreflightRetireAsync(
        ZLinkRetirePreflightPlan plan,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken)
    {
        (ZLinkSpotActivation Activation, bool Instance)[] units;
        lock (_gate)
        {
            if (_spots.Count == 0)
                return ValueTask.FromResult<ZLinkFrameworkRelocationReason?>(null);
            if (_retireScheduler is null)
                return ValueTask.FromResult<ZLinkFrameworkRelocationReason?>(
                    ZLinkFrameworkRelocationReason.RelocationDisabled);
            units = _spots.Values
                .Select(activation => (
                    activation,
                    _instanceSpotTypes.ContainsKey(activation.SpotId)))
                .ToArray();
        }
        return _retireScheduler.PreflightAsync(
            units,
            plan,
            selection,
            cancellationToken);
    }

    internal async ValueTask<ZLinkSpotDrainResult> TryRelocateForRetireAsync(
        ZLinkRelocationTargetSelection selection,
        ZLinkSpotRelocationPhase phase,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        cancellationToken.ThrowIfCancellationRequested();
        (ZLinkSpotActivation Activation, bool Instance)[] units;
        lock (_gate)
        {
            if (_spots.Count == 0)
                return new ZLinkSpotDrainResult(
                    true,
                    0,
                    null,
                    ZLinkRelocationCommitKnowledge.NotCommitted,
                    true);
            //  Preflight(같은 파일의 RelocationDisabled 분기)와 같은 판정이다.
            //  Scheduler가 없으면 다시 불러도 결과가 달라지지 않는다.
            if (_retireScheduler is null)
                return new ZLinkSpotDrainResult(
                    false,
                    0,
                    ZLinkFrameworkRelocationReason.RelocationDisabled,
                    ZLinkRelocationCommitKnowledge.NotCommitted,
                    true);
            units = _spots.Values
                .Select(activation => (
                    Activation: activation,
                    Instance: _instanceSpotTypes.ContainsKey(
                        activation.SpotId)))
                .Where(unit => phase == ZLinkSpotRelocationPhase.PerActorShells
                    ? !unit.Instance
                      && unit.Activation.ExecutionMode
                      == ZLinkUserSpotExecutionMode.PerActor
                    : unit.Instance
                      || unit.Activation.ExecutionMode
                      != ZLinkUserSpotExecutionMode.PerActor)
                .ToArray();
        }
        if (units.Length == 0)
            return new ZLinkSpotDrainResult(
                true,
                0,
                null,
                ZLinkRelocationCommitKnowledge.NotCommitted,
                true);

        var deadline = absoluteDeadline
                       ?? DateTimeOffset.UtcNow
                          + units.Select(static unit =>
                                  unit.Activation.DefaultRequestTimeout)
                              .DefaultIfEmpty(TimeSpan.FromSeconds(30))
                              .Max();
        var moves = units.Select(unit => RelocateAsync(unit).AsTask()).ToArray();
        var results = await Task.WhenAll(moves).ConfigureAwait(false);
        var committedUnitCount = checked((ulong)results.Sum(static result =>
            checked((long)result.CommittedUnitCount)));
        var terminal = results.FirstOrDefault(
            static result => result.Outcome
                == ZLinkRelocationUnitOutcome.TerminalFailure);
        var commitKnowledge = results.Any(static result =>
                result.CommitKnowledge
                == ZLinkRelocationCommitKnowledge.Unknown)
            ? ZLinkRelocationCommitKnowledge.Unknown
            : committedUnitCount != 0
                ? ZLinkRelocationCommitKnowledge.Committed
                : ZLinkRelocationCommitKnowledge.NotCommitted;
        return new ZLinkSpotDrainResult(
            results.All(static result => result.Outcome
                == ZLinkRelocationUnitOutcome.Completed),
            committedUnitCount,
            terminal.TerminalReason,
            commitKnowledge,
            results.All(static result => result.SourceTerminalized));

        async ValueTask<ZLinkRelocationUnitResult> RelocateAsync(
            (ZLinkSpotActivation Activation, bool Instance) unit)
        {
            try
            {
                return await _retireScheduler.TryRelocateAsync(
                        unit.Activation,
                        unit.Instance,
                        selection,
                        deadline,
                        CompleteRelocatedSourceAsync,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (ZLinkAuthorityGenerationExhaustedException)
            {
                throw;
            }
            catch (Exception exception)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"relocation_failed spot={unit.Activation.SpotId} "
                    + $"error={exception}");
                return ZLinkRelocationUnitResult.Terminal(
                    ZLinkFrameworkRelocationReason.RelocationFailed,
                    ZLinkRelocationCommitKnowledge.Unknown,
                    sourceTerminalized: false);
            }
        }
    }

    private async ValueTask CompleteRelocatedSourceAsync(
        ZLinkSpotActivation activation,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lifecycle?.SpotLocations.ForgetRelocated(
            activation.SpotId,
            activation.ObjectGeneration);
        lock (_gate)
        {
            _spots.Remove(activation.SpotId);
            _instanceSpotTypes.Remove(activation.SpotId);
            _closing.Remove(activation.SpotId);
        }
        await ScheduleRelocatedSourceCleanupAsync(runtime, activation)
            .ConfigureAwait(false);
    }

    internal static async ValueTask ScheduleRelocatedSourceCleanupAsync(
        ZLinkFrameworkRuntime runtime,
        ZLinkSpotActivation activation)
    {
        ArgumentNullException.ThrowIfNull(runtime);
        ArgumentNullException.ThrowIfNull(activation);
        var waitForPerActorMembers =
            activation.PerActorShellRelocationPlan is not null;
        async ValueTask CompleteAfterMessageFollowAsync(
            CancellationToken detachedCancellationToken)
        {
            try
            {
                var messageFollow = activation
                    .WaitForMessageFollowDrainedAsync(
                        detachedCancellationToken)
                    .AsTask();
                if (waitForPerActorMembers)
                    await activation
                        .InvokePerActorRelocationClosingAfterDrainAsync(
                            messageFollow,
                            detachedCancellationToken)
                        .ConfigureAwait(false);
                else
                    await messageFollow.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (detachedCancellationToken.IsCancellationRequested)
            {
            }
            finally
            {
                await activation.DisposeAsync().ConfigureAwait(false);
                ZLinkRuntimeMetrics.RecordSpotClosed(
                    activation.MeshName,
                    activation.Spot is IZLinkInstanceSpot
                        ? "instance"
                        : "user");
            }
        }

        if (!runtime.TryRunDetached(
                waitForPerActorMembers
                    ? "per-actor-shell-message-follow"
                    : "spot-message-follow-duration",
                CompleteAfterMessageFollowAsync))
            await CompleteAfterMessageFollowAsync(runtime.ShutdownToken)
                .ConfigureAwait(false);
    }

    private static ZLinkSpotRetireScheduler? CreateRetireScheduler(
        IServiceProvider services,
        ZLinkFrameworkRuntime runtime,
        ZLinkFrameworkRegistration registration)
    {
        var location = registration.Locations.ResolveStore();
        var relocation = registration.Locations.ResolveRelocationStore();
        var target = services.GetService<IZLinkSpotRetireTarget>();
        return location is null || relocation is null || target is null
            ? null
            : new ZLinkSpotRetireScheduler(
                location,
                relocation,
                target,
                runtime.RelocationPermits,
                runtime);
    }

    internal void RequestStop()
    {
        foreach (var activation in SnapshotActivations()) activation.RequestStop();
    }

    internal void CancelActiveOperations()
    {
        foreach (var activation in SnapshotActivations()) activation.CancelActiveOperations();
    }

    internal async ValueTask CloseLifecycleAsync()
    {
        await StopIdleEvictionAsync().ConfigureAwait(false);
        var activations = SnapshotActivations();
        List<Exception>? failures = null;
        foreach (var activation in activations)
        {
            var spotId = activation.SpotId;
            TaskCompletionSource<bool> transaction;
            bool ownsTransaction;
            lock (_gate)
            {
                if (!_spots.ContainsKey(spotId)) continue;
                if (_closing.TryGetValue(spotId, out transaction!))
                {
                    ownsTransaction = false;
                }
                else
                {
                    transaction = new TaskCompletionSource<bool>(
                        TaskCreationOptions.RunContinuationsAsynchronously);
                    _closing.Add(spotId, transaction);
                    ownsTransaction = true;
                }
            }

            await CaptureAsync(async () =>
                {
                    if (ownsTransaction)
                        _ = await ExecuteCloseTransactionAsync(
                                spotId,
                                activation,
                                transaction,
                                ZLinkSpotCloseReason.HostShutdown,
                                DateTimeOffset.UtcNow + activation.DefaultRequestTimeout)
                            .ConfigureAwait(false);
                    else
                        _ = await transaction.Task.ConfigureAwait(false);
                })
                .ConfigureAwait(false);

            bool stillTracked;
            lock (_gate) stillTracked = _spots.ContainsKey(spotId);
            if (stillTracked)
                await CaptureAsync(() => ForceCloseForShutdownAsync(activation)).ConfigureAwait(false);
        }

        if (failures is { Count: 1 })
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures is { Count: > 1 }) throw new AggregateException(failures);
        return;

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }
    }

    public ValueTask DisposeAsync()
    {
        lock (_disposeGate)
        {
            if (_disposeTask is not null) return new ValueTask(_disposeTask);

            Task creationsDrained;
            lock (_gate)
            {
                _closed = true;
                creationsDrained = _activeCreations == 0
                    ? Task.CompletedTask
                    : (_creationsDrained ??= new TaskCompletionSource(
                        TaskCreationOptions.RunContinuationsAsynchronously)).Task;
            }

            return new ValueTask(
                _disposeTask = DisposeCoreAsync(
                    creationsDrained,
                    forceStop: false));
        }
    }

    internal ValueTask ForceStopAsync()
    {
        lock (_disposeGate)
        {
            if (_disposeTask is not null) return new ValueTask(_disposeTask);

            Task creationsDrained;
            lock (_gate)
            {
                _closed = true;
                creationsDrained = _activeCreations == 0
                    ? Task.CompletedTask
                    : (_creationsDrained ??= new TaskCompletionSource(
                        TaskCreationOptions.RunContinuationsAsynchronously)).Task;
            }

            return new ValueTask(
                _disposeTask = DisposeCoreAsync(
                    creationsDrained,
                    forceStop: true));
        }
    }

    private async Task DisposeCoreAsync(
        Task creationsDrained,
        bool forceStop)
    {
        List<Exception>? failures = null;
        await CaptureAsync(StopIdleEvictionAsync).ConfigureAwait(false);
        await CaptureAsync(() => new ValueTask(creationsDrained)).ConfigureAwait(false);
        if (!forceStop)
            await CaptureAsync(CloseLifecycleAsync).ConfigureAwait(false);

        ZLinkSpotActivation[] activations;
        lock (_gate)
        {
            activations = _spots.Values.ToArray();
        }

        foreach (var activation in activations)
        {
            var spotId = activation.SpotId;
            await CaptureAsync(activation.DisposeAsync).ConfigureAwait(false);
            lock (_gate)
            {
                _spots.Remove(spotId);
                _instanceSpotTypes.Remove(spotId);
                _closing.Remove(spotId);
            }
        }

        if (failures is { Count: 1 })
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures is { Count: > 1 }) throw new AggregateException(failures);
        return;

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }
    }

    private async Task RunIdleEvictionAsync()
    {
        var interval = _instanceSpotIdleTimeout < TimeSpan.FromSeconds(1)
            ? _instanceSpotIdleTimeout
            : TimeSpan.FromSeconds(1);
        try
        {
            using var timer = new PeriodicTimer(interval);
            while (await timer.WaitForNextTickAsync(_idleEvictionStop.Token)
                       .ConfigureAwait(false))
            {
                foreach (var activation in SnapshotIdleEvictionCandidates())
                {
                    if (!IsIdleInstanceCandidate(activation)) continue;
                    var closed = await CloseCoreAsync(
                            activation.SpotId,
                            null,
                            releaseLocation: true,
                            requireNoActors: true,
                            ZLinkSpotCloseReason.IdleEvicted,
                            _idleEvictionStop.Token)
                        .ConfigureAwait(false);
                    if (!closed)
                        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"instance_spot_idle_eviction_deferred spot={activation.SpotId}");
                }
            }
        }
        catch (OperationCanceledException)
            when (_idleEvictionStop.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            runtime.ErrorSink.ReportRuntimeTaskException(
                "instance-spot-idle-eviction",
                exception);
        }
    }

    internal IReadOnlyList<ZLinkSpotActivation>
        SnapshotIdleEvictionCandidates()
    {
        lock (_gate)
        {
            if (_spots.Count == 0)
            {
                _idleEvictionCursor = null;
                return Array.Empty<ZLinkSpotActivation>();
            }

            var candidates = new List<ZLinkSpotActivation>(
                Math.Min(_spots.Count, IdleEvictionBatchSize));
            var cursor = _idleEvictionCursor;
            var cursorPresent = cursor is not null
                                && _spots.ContainsKey(cursor);
            var collecting = cursor is null || !cursorPresent;

            foreach (var (spotId, activation) in _spots)
            {
                if (!collecting)
                {
                    if (StringComparer.Ordinal.Equals(spotId, cursor))
                        collecting = true;
                    continue;
                }

                candidates.Add(activation);
                if (candidates.Count == IdleEvictionBatchSize)
                    break;
            }

            // The cursor can be near the end of the dictionary. Wrap once so
            // every activation receives a bounded inspection opportunity.
            if (candidates.Count < IdleEvictionBatchSize && cursorPresent)
            {
                foreach (var (spotId, activation) in _spots)
                {
                    if (StringComparer.Ordinal.Equals(spotId, cursor))
                    {
                        // Include the cursor itself only after all entries
                        // before it have been visited. This closes the cycle
                        // when the cursor is the last (or only) entry.
                        if (candidates.Count < IdleEvictionBatchSize)
                            candidates.Add(activation);
                        break;
                    }

                    candidates.Add(activation);
                    if (candidates.Count == IdleEvictionBatchSize)
                        break;
                }
            }

            _idleEvictionCursor = candidates.Count == 0
                ? cursor
                : candidates[^1].SpotId;
            return candidates.ToArray();
        }
    }

    private bool IsIdleInstanceCandidate(ZLinkSpotActivation activation)
    {
        if (_instanceSpotIdleTimeout <= TimeSpan.Zero
            || activation.Spot is not IZLinkInstanceSpot
            || activation.JoinedActorCount != 0
            || activation.HasIdleRelocationParticipation
            || activation.HasPendingApplicationWork)
            return false;

        var elapsed = Stopwatch.GetTimestamp()
                      - activation.LastApplicationWorkCompletedAt;
        var required = _instanceSpotIdleTimeout.TotalSeconds
                       * Stopwatch.Frequency;
        return elapsed >= required;
    }

    private async ValueTask StopIdleEvictionAsync()
    {
        if (Interlocked.Exchange(ref _idleEvictionStopped, 1) != 0)
            return;
        Task? task;
        lock (_disposeGate)
        {
            _idleEvictionStop.Cancel();
            task = _idleEvictionTask;
        }
        if (task is not null)
            await task.ConfigureAwait(false);
        _idleEvictionStop.Dispose();
    }

    public async ValueTask<ZLinkSpotCreateResult> CreateAsync(
        Type spotType,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            EnsureSpotTypeRegisteredLocked(spotType);
            EnsureLocalSpotCapacityLocked(spotType);
            BeginCreationLocked();
            IncrementGeneratedSpotCreationLocked(spotType);
        }

        IZLinkBackendSpot? nativeSpot = null;
        ZLinkSpotActivation? activation = null;
        try
        {
            var spotId = Guid.NewGuid().ToString("D");
            nativeSpot = node.GetOrCreateSpot(spotId, out var created);
            if (!created)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.AlreadyExists,
                    $"Generated User Spot ID '{spotId}' is already active.");
            var creation = await _activationFactory.CreateAsync(
                spotType,
                nativeSpot,
                spotId,
                request,
                cancellationToken);
            activation = creation.Activation;

            if (!creation.Response.Accepted)
            {
                var rejected = new ZLinkSpotCreateResult(
                    Reference(activation),
                    ZLinkSpotCreateState.Rejected,
                    creation.Response.Reply);
                await DisposeFailedCreationAsync(activation);
                return rejected;
            }

            cancellationToken.ThrowIfCancellationRequested();
            await ClaimSpotLocationAsync(activation, spotType, cancellationToken)
                .ConfigureAwait(false);
            lock (_gate)
            {
                _spots.Add(activation.SpotId, activation);
            }
            ZLinkRuntimeMetrics.RecordSpotCreated(registration.SpotNodeName, "user");

            return new ZLinkSpotCreateResult(
                Reference(activation),
                ZLinkSpotCreateState.Created,
                creation.Response.Reply);
        }
        catch (Exception error)
        {
            RemoveActivation(activation);
            var failures = new ZLinkFailureCollector(WrapSpotCreateFailed(spotType, error));
            if (activation is not null)
                await failures.CaptureAsync(activation.DisposeAsync).ConfigureAwait(false);
            failures.ThrowIfAny();
            throw new InvalidOperationException("Unreachable after creation cleanup failure propagation.");
        }
        finally
        {
            lock (_gate)
                DecrementGeneratedSpotCreationLocked(spotType);
            EndCreation();
        }
    }

    public async ValueTask<ZLinkSpotCreateResult> GetOrCreateAsync(
        Type spotType,
        string requestedSpotId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        PendingSpotCreation pending;
        var owner = false;
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            EnsureSpotTypeRegisteredLocked(spotType);
            EnsureCreationAdmissionOpenLocked();
            ThrowIfClosingLocked(requestedSpotId);

            if (_spots.TryGetValue(requestedSpotId, out var existing))
            {
                ThrowIfSpotTypeMismatch(existing.Spot.GetType(), spotType, requestedSpotId);
                return new ZLinkSpotCreateResult(
                    Reference(existing),
                    ZLinkSpotCreateState.Existing,
                    null);
            }

            if (_pending.TryGetValue(requestedSpotId, out pending!))
            {
                ThrowIfSpotTypeMismatch(pending.SpotType, spotType, requestedSpotId);
            }
            else
            {
                EnsureLocalSpotCapacityLocked(spotType);
                BeginCreationLocked();
                pending = new PendingSpotCreation(spotType);
                _pending.Add(requestedSpotId, pending);
                owner = true;
            }
        }

        if (owner)
            _ = CompleteReservedCreationAsync(
                spotType,
                requestedSpotId,
                request,
                pending,
                runtime.ShutdownToken,
                claimLegacyLocation: true);

        var result = await pending.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        return !owner && result.State == ZLinkSpotCreateState.Created
            ? result with { State = ZLinkSpotCreateState.Existing }
            : result;
    }

    private async ValueTask CompleteReservedCreationAsync(
        Type spotType,
        string requestedSpotId,
        ZLinkMessage request,
        PendingSpotCreation pending,
        CancellationToken cancellationToken,
        bool claimLegacyLocation)
    {
        IZLinkBackendSpot? nativeSpot = null;
        ZLinkSpotActivation? activation = null;
        var factoryOwnsNativeSpot = false;
        try
        {
            nativeSpot = node.GetOrCreateSpot(requestedSpotId, out var created);
            if (!created)
            {
                var existingNativeSpot = nativeSpot;
                nativeSpot = null;
                await existingNativeSpot.DisposeAsync();
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InternalFailure,
                    $"SPOT routing id '{requestedSpotId}' already exists in core but no framework SPOT is registered.");
            }

            factoryOwnsNativeSpot = true;
            var creation = await _activationFactory.CreateAsync(
                spotType,
                nativeSpot,
                requestedSpotId,
                request,
                cancellationToken);
            activation = creation.Activation;

            if (!creation.Response.Accepted)
            {
                var rejected = new ZLinkSpotCreateResult(
                    Reference(activation),
                    ZLinkSpotCreateState.Rejected,
                    creation.Response.Reply);
                await DisposeFailedCreationAsync(activation).ConfigureAwait(false);
                lock (_gate)
                {
                    _pending.Remove(requestedSpotId);
                    pending.Complete(rejected);
                }
                return;
            }

            cancellationToken.ThrowIfCancellationRequested();
            if (claimLegacyLocation)
                await ClaimSpotLocationAsync(activation, spotType, cancellationToken)
                    .ConfigureAwait(false);

            var result = new ZLinkSpotCreateResult(
                Reference(activation),
                ZLinkSpotCreateState.Created,
                creation.Response.Reply);
            lock (_gate)
            {
                _pending.Remove(requestedSpotId);
                _spots.Add(activation.SpotId, activation);
                pending.Complete(result);
            }
            ZLinkRuntimeMetrics.RecordSpotCreated(registration.SpotNodeName, "user");
        }
        catch (Exception error)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"SPOT '{requestedSpotId}' creation failed on node '{node.RoutingId}': {error}");
            var wrapped = WrapSpotCreateFailed(spotType, error);
            lock (_gate)
            {
                RemoveActivationLocked(activation);
            }

            var failures = new ZLinkFailureCollector(wrapped);
            if (activation is not null)
                await failures.CaptureAsync(activation.DisposeAsync).ConfigureAwait(false);
            else if (!factoryOwnsNativeSpot && nativeSpot is not null)
                await failures.CaptureAsync(nativeSpot.DisposeAsync).ConfigureAwait(false);
            var finalFailure = failures.BuildException()!;
            lock (_gate)
            {
                _pending.Remove(requestedSpotId);
                pending.Fail(finalFailure);
            }
        }
        finally
        {
            EndCreation();
        }
    }

    internal async ValueTask<PreparedReservedSpot> PrepareReservedAsync(
        Type spotType,
        string requestedSpotId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkMessage request,
        CancellationToken cancellationToken,
        bool invokeCreate = true)
    {
        ArgumentNullException.ThrowIfNull(request);
        var pending = new PendingSpotCreation(spotType);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            EnsureSpotTypeRegisteredLocked(spotType);
            EnsureCreationAdmissionOpenLocked();
            ThrowIfClosingLocked(requestedSpotId);
            if (_spots.TryGetValue(requestedSpotId, out var existing))
            {
                ThrowIfSpotTypeMismatch(existing.Spot.GetType(), spotType, requestedSpotId);
                return new PreparedReservedSpot(existing, true, null);
            }
            if (_pending.ContainsKey(requestedSpotId))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"SPOT '{requestedSpotId}' is already being materialized.",
                    ZLinkRetryAdvice.RetryAfterBackoff);

            EnsureLocalSpotCapacityLocked(spotType);
            BeginCreationLocked();
            _pending.Add(requestedSpotId, pending);
        }

        IZLinkBackendSpot? nativeSpot = null;
        ZLinkSpotActivation? activation = null;
        try
        {
            nativeSpot = node.GetOrCreateReservedSpot(
                requestedSpotId,
                objectGeneration,
                authorityOwnerGeneration,
                out var created);
            if (!created)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"SPOT '{requestedSpotId}' is already materialized.");
            ZLinkSpotCreateResponse? response;
            if (invokeCreate)
            {
                var creation = await _activationFactory.CreateAsync(
                        spotType,
                        nativeSpot,
                        requestedSpotId,
                        request,
                        cancellationToken)
                    .ConfigureAwait(false);
                activation = creation.Activation;
                response = creation.Response;
            }
            else
            {
                activation = await _activationFactory.CreateForRelocationAsync(
                        spotType,
                        nativeSpot,
                        requestedSpotId,
                        cancellationToken)
                    .ConfigureAwait(false);
                response = null;
            }
            lock (_gate)
            {
                _pending.Remove(requestedSpotId);
                _preparedSpotTypes.Add(
                    requestedSpotId,
                    activation!.Spot.GetType());
            }
            EndCreation();
            return new PreparedReservedSpot(
                activation,
                false,
                response);
        }
        catch
        {
            lock (_gate)
            {
                _pending.Remove(requestedSpotId);
                _preparedSpotTypes.Remove(requestedSpotId);
            }
            if (activation is not null)
                await activation.DisposeAsync().ConfigureAwait(false);
            else if (nativeSpot is not null)
                await nativeSpot.DisposeAsync().ConfigureAwait(false);
            EndCreation();
            throw;
        }
    }

    internal async ValueTask<PreparedReservedSpot> PrepareInstanceReservedAsync(
        string stableType,
        string requestedSpotId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken,
        bool restoreLogicalTimers = false)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!registration.InstanceSpotFactories.TryGetValue(
                stableType,
                out var factory))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                $"Instance Spot type '{stableType}' is not registered.");

        var pending = new PendingSpotCreation(factory.SpotType);
        lock (_gate)
        {
            EnsureCreationAdmissionOpenLocked();
            ThrowIfClosingLocked(requestedSpotId);
            if (_spots.TryGetValue(requestedSpotId, out var existing))
            {
                ThrowIfSpotTypeMismatch(
                    existing.Spot.GetType(),
                    factory.SpotType,
                    requestedSpotId);
                return new PreparedReservedSpot(
                    existing,
                    true,
                    null,
                    stableType);
            }
            if (_pending.ContainsKey(requestedSpotId))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{requestedSpotId}' is already being materialized.",
                    ZLinkRetryAdvice.RetryAfterBackoff);

            EnsureLocalSpotCapacityLocked(factory.SpotType);
            BeginCreationLocked();
            _pending.Add(requestedSpotId, pending);
        }

        IZLinkBackendSpot? nativeSpot = null;
        ZLinkSpotActivation? activation = null;
        try
        {
            nativeSpot = node.GetOrCreateReservedSpot(
                requestedSpotId,
                objectGeneration,
                authorityOwnerGeneration,
                out var created);
            if (!created)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{requestedSpotId}' is already materialized.");
            activation = await _activationFactory.CreateInstanceAsync(
                    factory.SpotType,
                    nativeSpot,
                    requestedSpotId,
                    cancellationToken,
                    restoreLogicalTimers)
                .ConfigureAwait(false);
            lock (_gate)
            {
                _pending.Remove(requestedSpotId);
                _preparedSpotTypes.Add(
                    requestedSpotId,
                    activation!.Spot.GetType());
            }
            EndCreation();
            return new PreparedReservedSpot(
                activation,
                false,
                null,
                stableType);
        }
        catch
        {
            lock (_gate)
            {
                _pending.Remove(requestedSpotId);
                _preparedSpotTypes.Remove(requestedSpotId);
            }
            if (activation is not null)
                await activation.DisposeAsync().ConfigureAwait(false);
            else if (nativeSpot is not null)
                await nativeSpot.DisposeAsync().ConfigureAwait(false);
            EndCreation();
            throw;
        }
    }

    internal async ValueTask PublishReservedAsync(
        PreparedReservedSpot prepared,
        string stableType,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        if (prepared.Existing) return;
        await ClaimSpotLocationAsync(
                prepared.Activation,
                stableType,
                objectGeneration,
                authorityOwnerGeneration,
                ZLinkSpotKind.User,
                cancellationToken)
            .ConfigureAwait(false);
        lock (_gate)
        {
            if (_spots.ContainsKey(prepared.Activation.SpotId))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"SPOT '{prepared.Activation.SpotId}' became visible before publication.");
            _preparedSpotTypes.Remove(prepared.Activation.SpotId);
            _spots.Add(prepared.Activation.SpotId, prepared.Activation);
        }
        ZLinkRuntimeMetrics.RecordSpotCreated(registration.SpotNodeName, "user");
    }

    internal ValueTask ValidateRelocatedReservedAsync(
        PreparedReservedSpot prepared,
        string stableType,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken) =>
        prepared.Existing || lifecycle is null
            ? ValueTask.CompletedTask
            : ValidateRelocatedReservedCoreAsync(
                prepared,
                stableType,
                objectGeneration,
                authorityOwnerGeneration,
                cancellationToken);

    private async ValueTask ValidateRelocatedReservedCoreAsync(
        PreparedReservedSpot prepared,
        string stableType,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        var activation = prepared.Activation;
        var kind = activation.Spot is IZLinkInstanceSpot
            ? ZLinkSpotKind.Instance
            : ZLinkSpotKind.User;
        var status = await lifecycle!.SpotLocations.TrackRelocatedAsync(
                spotChannelName,
                activation.SpotId,
                objectGeneration,
                authorityOwnerGeneration,
                stableType,
                node.RoutingId,
                node.MeshStatus().LifecycleGeneration,
                kind,
                deactivate: async ct =>
                    _ = await CloseAsync(activation.SpotId, ct)
                        .ConfigureAwait(false),
                cancellationToken)
            .ConfigureAwait(false);
        if (status != ZLinkLocationWriteStatus.Stored)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"SPOT '{activation.SpotId}' relocation authority is not published for this target.",
                ZLinkRetryAdvice.RetryAfterBackoff);
    }

    internal void PublishRelocatedReserved(PreparedReservedSpot prepared)
    {
        if (prepared.Existing) return;
        lock (_gate)
        {
            if (_spots.ContainsKey(prepared.Activation.SpotId))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"SPOT '{prepared.Activation.SpotId}' became visible before relocation publication.");
            _preparedSpotTypes.Remove(prepared.Activation.SpotId);
            _spots.Add(prepared.Activation.SpotId, prepared.Activation);
            if (prepared.Activation.Spot is IZLinkInstanceSpot)
                _instanceSpotTypes.Add(
                    prepared.Activation.SpotId,
                    prepared.InstanceStableType
                    ?? throw new InvalidOperationException(
                        "Instance Spot stable type is missing."));
        }
        ZLinkRuntimeMetrics.RecordSpotCreated(
            registration.SpotNodeName,
            prepared.Activation.Spot is IZLinkInstanceSpot
                ? "instance"
                : "user");
    }

    internal async ValueTask PublishInstanceReservedAsync(
        PreparedReservedSpot prepared,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        if (prepared.Existing) return;
        await ClaimSpotLocationAsync(
                prepared.Activation,
                prepared.InstanceStableType,
                objectGeneration,
                authorityOwnerGeneration,
                ZLinkSpotKind.Instance,
                cancellationToken)
            .ConfigureAwait(false);
        lock (_gate)
        {
            if (_spots.ContainsKey(prepared.Activation.SpotId))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{prepared.Activation.SpotId}' became visible before publication.");
            _preparedSpotTypes.Remove(prepared.Activation.SpotId);
            _spots.Add(prepared.Activation.SpotId, prepared.Activation);
            _instanceSpotTypes.Add(
                prepared.Activation.SpotId,
                prepared.InstanceStableType
                ?? throw new InvalidOperationException(
                    "Instance Spot stable type is missing."));
        }
        ZLinkRuntimeMetrics.RecordSpotCreated(registration.SpotNodeName, "instance");
    }

    internal bool TryGetInstanceActivation(
        string spotId,
        string stableType,
        ulong objectGeneration,
        out ZLinkSpotActivation activation)
    {
        lock (_gate)
        {
            if (!_closing.ContainsKey(spotId)
                && _spots.TryGetValue(spotId, out var existing)
                && existing.Spot is IZLinkInstanceSpot
                && existing.NativeSpot.LifecycleGeneration == objectGeneration
                && registration.InstanceSpotFactories.TryGetValue(
                    stableType,
                    out var factory)
                && factory.SpotType.IsInstanceOfType(existing.Spot))
            {
                activation = existing;
                return true;
            }
        }

        activation = null!;
        return false;
    }

    internal bool TryGetPerActorRelocationShell(
        string spotId,
        ulong objectGeneration,
        RoutingId nodeRid,
        ulong nodeLifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        out ZLinkSpotActivation activation)
    {
        lock (_gate)
        {
            if (!_closing.ContainsKey(spotId)
                && _spots.TryGetValue(spotId, out var existing)
                && existing.Spot is not IZLinkInstanceSpot
                && existing.ExecutionMode == ZLinkUserSpotExecutionMode.PerActor
                && existing.ObjectGeneration == objectGeneration
                && existing.NodeRid == nodeRid
                && existing.SourceNodeLifecycleGeneration
                   == nodeLifecycleGeneration
                && existing.SourceOwnerToken == owner)
            {
                activation = existing;
                return true;
            }
        }

        activation = null!;
        return false;
    }

    internal async ValueTask DiscardReservedAsync(PreparedReservedSpot prepared)
    {
        if (prepared.Existing) return;
        lock (_gate) _preparedSpotTypes.Remove(prepared.Activation.SpotId);
        await prepared.Activation.DisposeAsync().ConfigureAwait(false);
    }

    internal bool HasActiveActors(string spotId)
    {
        lock (_gate)
            return _spots.TryGetValue(spotId, out var activation)
                   && activation.JoinedActorCount > 0;
    }

    internal ReservedSpotCloseReadiness CloseReadiness(string spotId)
    {
        lock (_gate)
        {
            if (!_spots.TryGetValue(spotId, out var activation))
                return ReservedSpotCloseReadiness.LocalMissing;
            if (_closing.ContainsKey(spotId))
                return ReservedSpotCloseReadiness.Closing;
            return activation.JoinedActorCount == 0
                ? ReservedSpotCloseReadiness.Ready
                : ReservedSpotCloseReadiness.HasActors;
        }
    }

    private static SpotRef Reference(ZLinkSpotActivation activation) =>
        new(
            activation.SpotId,
            activation.NativeSpot.LifecycleGeneration,
            activation.SpotNodeName,
            activation.NodeRid);

    public ValueTask<ZLinkSpotInfo?> GetAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            ZLinkSpotInfo? result = _spots.TryGetValue(spotId, out var activation)
                ? new ZLinkSpotInfo(activation.SpotId)
                : null;
            return ValueTask.FromResult(result);
        }
    }

    public ValueTask<IReadOnlyList<ZLinkSpotInfo>> ListAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            IReadOnlyList<ZLinkSpotInfo> result = _spots.Values
                .Select(static activation => new ZLinkSpotInfo(activation.SpotId))
                .OrderBy(static item => item.SpotId, StringComparer.Ordinal)
                .ToArray();
            return ValueTask.FromResult(result);
        }
    }

    public async ValueTask<bool> CloseAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        return await CloseAsync(spotId, null, cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<bool> CloseAsync(
        string spotId,
        DateTimeOffset? deadline,
        CancellationToken cancellationToken)
    {
        return await CloseCoreAsync(
                spotId,
                deadline,
                releaseLocation: true,
                requireNoActors: true,
                ZLinkSpotCloseReason.ExplicitClose,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal ValueTask<bool> CloseReservedAsync(
        string spotId,
        DateTimeOffset? deadline,
        CancellationToken cancellationToken) =>
        CloseCoreAsync(
            spotId,
            deadline,
            releaseLocation: false,
            requireNoActors: true,
            ZLinkSpotCloseReason.ExplicitClose,
            cancellationToken);

    private async ValueTask<bool> CloseCoreAsync(
        string spotId,
        DateTimeOffset? deadline,
        bool releaseLocation,
        bool requireNoActors,
        ZLinkSpotCloseReason reason,
        CancellationToken cancellationToken)
    {
        ZLinkSpotActivation? activation;
        TaskCompletionSource<bool>? transaction;
        var ownsTransaction = false;
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (_closing.TryGetValue(spotId, out transaction))
            {
                activation = null;
            }
            else
            {
                if (!_spots.TryGetValue(spotId, out activation)) return false;

                transaction = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                _closing.Add(spotId, transaction);
                ownsTransaction = true;
            }
        }

        if (!ownsTransaction)
            return await transaction!.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

        if (ReferenceEquals(ZLinkSpotAmbientContext.CurrentOrDefault, activation))
        {
            if (!runtime.TryRunDetached(
                    "spot-close-after-current-turn",
                    async _ =>
                    {
                        await ExecuteCloseTransactionAsync(
                                spotId,
                                activation!,
                                transaction!,
                                reason,
                                deadline,
                                releaseLocation,
                                requireNoActors)
                            .ConfigureAwait(false);
                    }))
            {
                lock (_gate) _closing.Remove(spotId);
                transaction!.TrySetException(new InvalidOperationException(
                    $"SPOT '{spotId}' close could not be scheduled in the current runtime generation."));
                return false;
            }

            return true;
        }

        return await ExecuteCloseTransactionAsync(
                spotId,
                activation!,
                transaction!,
                reason,
                deadline,
                releaseLocation,
                requireNoActors)
            .ConfigureAwait(false);
    }

    /// <summary>A created User Spot claims its authority before it becomes
    /// addressable. A claim that cannot be stored fails creation because an
    /// unadvertised or doubly claimed Spot would break single activation.</summary>
    private async ValueTask ClaimSpotLocationAsync(
        ZLinkSpotActivation activation,
        Type spotType,
        CancellationToken cancellationToken)
    {
        await ClaimSpotLocationAsync(
                activation,
                spotType.FullName,
                activation.NativeSpot.LifecycleGeneration,
                authorityOwnerGeneration: 0,
                ZLinkSpotKind.User,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ClaimSpotLocationAsync(
        ZLinkSpotActivation activation,
        string? spotType,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkSpotKind spotKind,
        CancellationToken cancellationToken)
    {
        if (lifecycle is null) return;

        var spotId = activation.SpotId;
        var status = await lifecycle.SpotLocations.ClaimAsync(
                spotChannelName,
                spotId,
                objectGeneration,
                spotType,
                node.RoutingId,
                node.MeshStatus().LifecycleGeneration,
                spotKind,
                authorityOwnerGeneration,
                deactivate: async ct => _ = await CloseAsync(spotId, ct).ConfigureAwait(false),
                cancellationToken)
            .ConfigureAwait(false);
        if (status == ZLinkLocationWriteStatus.Stored) return;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InternalFailure,
            status == ZLinkLocationWriteStatus.RejectedConflict
                ? $"SPOT '{spotId}' location in mesh '{spotChannelName}' is owned by another node."
                : $"SPOT '{spotId}' location claim failed because the location store is unavailable.");
    }

    private async ValueTask ReleaseSpotLocationAsync(string spotId)
    {
        if (lifecycle is not null)
            await lifecycle.SpotLocations.ReleaseAsync(spotChannelName, spotId).ConfigureAwait(false);
    }

    private IReadOnlyCollection<ZLinkSpotActivation> SnapshotActivations()
    {
        lock (_gate)
        {
            return _spots.Values.ToArray();
        }
    }

    private async ValueTask<bool> ExecuteCloseTransactionAsync(
        string spotId,
        ZLinkSpotActivation activation,
        TaskCompletionSource<bool> transaction,
        ZLinkSpotCloseReason reason,
        DateTimeOffset? deadline,
        bool releaseLocation = true,
        bool requireNoActors = true)
    {
        try
        {
            if (!await activation.TryCloseIfNoActorsAsync(
                    reason,
                    deadline ?? DateTimeOffset.UtcNow + activation.DefaultRequestTimeout,
                    requireNoActors,
                    CancellationToken.None)
                    .ConfigureAwait(false))
            {
                lock (_gate) _closing.Remove(spotId);
                transaction.TrySetResult(false);
                return false;
            }

            await activation.DisposeAsync().ConfigureAwait(false);
            if (releaseLocation)
                await ReleaseSpotLocationAsync(spotId).ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            lock (_gate)
            {
                _spots.Remove(spotId);
                _instanceSpotTypes.Remove(spotId);
                _closing.Remove(spotId);
            }
            transaction.TrySetException(exception);
            throw;
        }

        lock (_gate)
        {
            _spots.Remove(spotId);
            _instanceSpotTypes.Remove(spotId);
            _closing.Remove(spotId);
        }
        ZLinkRuntimeMetrics.RecordSpotClosed(
            registration.SpotNodeName,
            activation.Spot is IZLinkInstanceSpot ? "instance" : "user");

        transaction.TrySetResult(true);
        return true;
    }

    internal static async ValueTask CloseBeforeReleaseAsync(
        Func<ValueTask> closeSpot,
        Func<ValueTask> releaseLocation)
    {
        await closeSpot().ConfigureAwait(false);
        await releaseLocation().ConfigureAwait(false);
    }

    private async ValueTask ForceCloseForShutdownAsync(ZLinkSpotActivation activation)
    {
        var failures = new ZLinkFailureCollector();
        await failures.CaptureAsync(
                () => activation.CloseAsync(
                    ZLinkSpotCloseReason.HostShutdown,
                    DateTimeOffset.UtcNow + activation.DefaultRequestTimeout,
                    CancellationToken.None))
            .ConfigureAwait(false);
        await failures.CaptureAsync(() => ReleaseSpotLocationAsync(activation.SpotId)).ConfigureAwait(false);
        await failures.CaptureAsync(activation.DisposeAsync).ConfigureAwait(false);
        lock (_gate)
        {
            _spots.Remove(activation.SpotId);
            _instanceSpotTypes.Remove(activation.SpotId);
            _closing.Remove(activation.SpotId);
        }
        ZLinkRuntimeMetrics.RecordSpotClosed(
            registration.SpotNodeName,
            activation.Spot is IZLinkInstanceSpot ? "instance" : "user");
        failures.ThrowIfAny();
    }

    private void RemoveActivation(ZLinkSpotActivation? activation)
    {
        if (activation is null) return;

        lock (_gate)
        {
            RemoveActivationLocked(activation);
        }
    }

    private void RemoveActivationLocked(ZLinkSpotActivation? activation)
    {
        if (activation is not null)
        {
            _spots.Remove(activation.SpotId);
            _instanceSpotTypes.Remove(activation.SpotId);
        }
    }

    private static async ValueTask DisposeFailedCreationAsync(ZLinkSpotActivation activation)
    {
        await activation.DisposeAsync();
    }

    private void ThrowIfClosingLocked(string spotId)
    {
        if (_closing.ContainsKey(spotId))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"SPOT '{spotId}' is being closed.",
                ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private void EnsureLocalSpotCapacityLocked(Type spotType)
    {
        var total = checked(
            _spots.Count
            + _pending.Count
            + _preparedSpotTypes.Count
            + _generatedSpotCreations.Values.Sum());
        if (registration.SpotLimit > 0
            && total >= registration.SpotLimit)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.CapacityExceeded,
                $"SPOT node '{registration.SpotNodeName}' reached its local spot limit.",
                ZLinkRetryAdvice.RetryAfterBackoff);

        if (!TryGetStableTypeLimit(spotType, out var stableType, out var limit)
            || limit <= 0)
            return;

        var typeTotal = 0;
        foreach (var activation in _spots.Values)
        {
            if (TryGetStableTypeLimit(
                    activation.Spot.GetType(),
                    out var currentType,
                    out _)
                && StringComparer.Ordinal.Equals(currentType, stableType))
                typeTotal++;
        }
        foreach (var pending in _pending.Values)
        {
            if (TryGetStableTypeLimit(
                    pending.SpotType,
                    out var currentType,
                    out _)
                && StringComparer.Ordinal.Equals(currentType, stableType))
                typeTotal++;
        }
        foreach (var prepared in _preparedSpotTypes.Values)
        {
            if (TryGetStableTypeLimit(
                    prepared,
                    out var currentType,
                    out _)
                && StringComparer.Ordinal.Equals(currentType, stableType))
                typeTotal++;
        }
        foreach (var (creatingType, count) in _generatedSpotCreations)
        {
            if (TryGetStableTypeLimit(
                    creatingType,
                    out var currentType,
                    out _)
                && StringComparer.Ordinal.Equals(currentType, stableType))
                typeTotal = checked(typeTotal + count);
        }

        if (typeTotal >= limit)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.CapacityExceeded,
                $"SPOT stable type '{stableType}' reached its local activation limit.",
                ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private bool IsStableTypeLocked(Type spotType, string stableType)
    {
        return TryGetStableTypeLimit(
                   spotType,
                   out var currentType,
                   out _)
               && StringComparer.Ordinal.Equals(currentType, stableType);
    }

    private void IncrementGeneratedSpotCreationLocked(Type spotType)
    {
        _generatedSpotCreations.TryGetValue(spotType, out var count);
        _generatedSpotCreations[spotType] = checked(count + 1);
    }

    private void DecrementGeneratedSpotCreationLocked(Type spotType)
    {
        if (!_generatedSpotCreations.TryGetValue(spotType, out var count)
            || count <= 0)
            throw new InvalidOperationException(
                "Generated SPOT creation accounting became inconsistent.");
        if (count == 1)
            _generatedSpotCreations.Remove(spotType);
        else
            _generatedSpotCreations[spotType] = count - 1;
    }

    private bool TryGetStableTypeLimit(
        Type spotType,
        out string stableType,
        out int limit)
    {
        foreach (var (registeredStableType, relocation) in
                 registration.InstanceSpotRelocations)
        {
            if (relocation.InstanceType != spotType) continue;
            stableType = registeredStableType;
            limit = relocation.Placement.MaxActiveObjects ?? 0;
            return true;
        }
        foreach (var (registeredStableType, relocation) in registration.SpotRelocations)
        {
            if (relocation.InstanceType != spotType) continue;
            stableType = registeredStableType;
            limit = relocation.Placement.MaxActiveObjects ?? 0;
            return true;
        }

        stableType = string.Empty;
        limit = 0;
        return false;
    }

    private void EnsureSpotTypeRegisteredLocked(Type spotType)
    {
        if (!registration.SpotFactories.Contains(spotType))
            throw new ZLinkConfigurationException(
                $"SPOT factory '{spotType}' is not registered on node '{registration.SpotNodeName}'.");
    }

    private void BeginCreationLocked()
    {
        EnsureCreationAdmissionOpenLocked();
        _activationAdmission.Acquire(
            $"SPOT node '{registration.SpotNodeName}'");
        _activeCreations++;
    }

    private void EnsureCreationAdmissionOpenLocked()
    {
        ObjectDisposedException.ThrowIf(_closed, this);
    }

    private void EndCreation()
    {
        TaskCompletionSource? drained = null;
        lock (_gate)
        {
            if (--_activeCreations < 0)
                throw new InvalidOperationException("SPOT creation admission count became negative.");
            if (_closed && _activeCreations == 0)
            {
                drained = _creationsDrained;
                _creationsDrained = null;
            }
        }

        _activationAdmission.Release();
        drained?.TrySetResult();
    }

    private static void ThrowIfSpotTypeMismatch(
        Type existingSpotType,
        Type requestedSpotType,
        string spotId)
    {
        if (existingSpotType == requestedSpotType) return;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.TypeMismatch,
            $"SPOT routing id '{spotId}' already belongs to '{existingSpotType}'.");
    }

    private static Exception WrapSpotCreateFailed(
        Type spotType,
        Exception error)
    {
        if (error is OperationCanceledException) return error;
        if (error is ZLinkFrameworkException frameworkError) return frameworkError;

        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InternalFailure,
            $"SPOT '{spotType}' creation failed.",
            innerException: error);
    }

    private sealed class PendingSpotCreation(Type spotType)
    {
        private readonly TaskCompletionSource<ZLinkSpotCreateResult> _completion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public Type SpotType { get; } = spotType;

        public Task<ZLinkSpotCreateResult> Task => _completion.Task;

        public void Complete(ZLinkSpotCreateResult result)
        {
            _completion.TrySetResult(result);
        }

        public void Fail(Exception error)
        {
            _completion.TrySetException(error);
        }
    }
}

internal readonly record struct ZLinkSpotDrainResult(
    bool Completed,
    ulong CommittedUnitCount,
    //  `Completed=false`만으로는 "아직 남았으니 다시 부르라"와 "구성상 영원히
    //  완료될 수 없다"를 구분할 수 없다. 후자를 이유와 함께 올려보내지 않으면
    //  호출자의 재시도 loop가 deadline까지 돈다.
    ZLinkFrameworkRelocationReason? TerminalReason = null,
    ZLinkRelocationCommitKnowledge CommitKnowledge =
        ZLinkRelocationCommitKnowledge.NotCommitted,
    bool SourceTerminalized = false)
{
    internal bool HasCommitted => CommittedUnitCount != 0;

    internal bool HasUnknownCommit =>
        CommitKnowledge == ZLinkRelocationCommitKnowledge.Unknown;
}

internal sealed record PreparedReservedSpot(
    ZLinkSpotActivation Activation,
    bool Existing,
    ZLinkSpotCreateResponse? Response,
    string? InstanceStableType = null)
{
    internal Type SpotType => Activation.Spot.GetType();
}

internal enum ReservedSpotCloseReadiness
{
    Ready,
    HasActors,
    LocalMissing,
    Closing
}
