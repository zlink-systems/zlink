using System.Diagnostics;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotSerialExecutor : IAsyncDisposable
{
    private readonly ZLinkSpotActivation _activation;
    private readonly Func<bool> _isDisposed;
    private readonly Func<bool> _flowCaptureEnabled;
    private readonly ZLinkSerialExecutionQueue _queue;
    private readonly ZLinkUserSpotExecutionMode _executionMode;
    private readonly ZLinkStateLane _stateLane = new();
    private readonly Dictionary<ZLinkActorId, ZLinkSerialExecutionQueue> _actorLanes = [];
    private readonly Dictionary<ZLinkTimerName, ZLinkSerialExecutionQueue> _timerLanes = [];
    private ZLinkSerialExecutionQueue[]? _completedChildLanes;
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly CancellationToken _stopToken;
    private readonly object _executionOwner;
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private readonly ZLinkExecutionLanePolicy _spotLanePolicy;
    private readonly ZLinkExecutionLanePolicy _actorLanePolicy;
    private readonly ZLinkExecutionLanePolicy _timerLanePolicy;
    private ZLinkExecutionBarrierState? _relocationBarrier;
    private ZLinkRelocationAdmissionOpeningState? _relocationAdmissionOpening;
    private bool _relocationAdmissionQueueOpened;
    private ulong _nextBarrierGeneration = 1;
    private int _activeApplicationClaims;
    private int _activeActorClaims;
    private int _spotStopping;
    private int _stopping;
    private long _lastApplicationWorkCompletedAt;
    private static readonly AsyncLocal<ZLinkRelocationAdmissionOpeningScope?>
        CurrentRelocationAdmissionOpening = new();

    public ZLinkSpotSerialExecutor(
        ZLinkSpotActivation activation,
        Func<bool> isDisposed,
        CancellationToken stopToken,
        IZLinkRuntimeFailureReporter errorSink,
        Func<bool>? flowCaptureEnabled = null,
        object? executionOwner = null,
        ZLinkUserSpotExecutionMode executionMode = ZLinkUserSpotExecutionMode.SpotWide,
        ZLinkExecutionLanePolicy? spotLanePolicy = null,
        ZLinkExecutionLanePolicy? actorLanePolicy = null,
        ZLinkExecutionLanePolicy? timerLanePolicy = null)
    {
        _activation = activation;
        _isDisposed = isDisposed;
        _flowCaptureEnabled = flowCaptureEnabled ?? AlwaysDisabled;
        _executionMode = executionMode;
        _errorSink = errorSink;
        _stopToken = stopToken;
        _spotLanePolicy = spotLanePolicy ?? ZLinkExecutionLanePolicy.Default;
        _actorLanePolicy = actorLanePolicy ?? ZLinkExecutionLanePolicy.Default;
        _timerLanePolicy = timerLanePolicy ?? ZLinkExecutionLanePolicy.Default;
        _executionOwner = executionOwner ?? activation?.RuntimeExecutionOwner ?? new object();
        _taskRunner = new ZLinkRuntimeTaskRunner(
            _errorSink,
            _stopToken,
            _executionOwner);
        _queue = CreateQueue(_spotLanePolicy);
        _lastApplicationWorkCompletedAt = Stopwatch.GetTimestamp();
    }

    private ZLinkSerialExecutionQueue CreateQueue(
        ZLinkExecutionLanePolicy policy)
    {
        return new ZLinkSerialExecutionQueue(
            _taskRunner,
            _errorSink,
            _stopToken,
            policy);
    }

    private static bool AlwaysDisabled() => false;

    public async ValueTask DisposeAsync()
    {
        Interlocked.Exchange(ref _spotStopping, 1);
        Interlocked.Exchange(ref _stopping, 1);
        await _queue.DisposeAsync().ConfigureAwait(false);
        var lanes = await _stateLane.RunAsync(CompleteChildLanesOnStateLane)
            .ConfigureAwait(false);
        foreach (var lane in lanes)
            await lane.DisposeAsync().ConfigureAwait(false);
        await _stateLane.DisposeAsync().ConfigureAwait(false);
        await _taskRunner.StopAsync().ConfigureAwait(false);
    }

    public void RequestStop()
    {
        Interlocked.Exchange(ref _spotStopping, 1);
        Interlocked.Exchange(ref _stopping, 1);
        _queue.Complete();
        CompleteChildLanes();
    }

    public async ValueTask ExecuteAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        if (Volatile.Read(ref _spotStopping) != 0 || _isDisposed()) return;

        var claim = AcquireApplicationClaim(actorLane: false);
        await RunClaimedAsync(
                _queue,
                ct => ExecuteOperationAsync(operation, null, ct),
                claim,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public ValueTask ExecuteActorAsync<TState>(
        string actorId,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
        => ExecuteActorAsync(
            actorId,
            operation,
            state,
            payloadBytes: 0,
            metadataBytes: 0,
            transferred: false,
            cancellationToken);

    internal ValueTask ExecuteActorAsync<TState>(
        string actorId,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        if (Volatile.Read(ref _stopping) != 0 || _isDisposed())
            return ValueTask.CompletedTask;
        var (lane, claim) = AcquireActorApplicationAdmission(actorId);
        return RunClaimedAsync(
            lane,
            ct => _executionMode == ZLinkUserSpotExecutionMode.SpotWide
                ? _queue.RunAsync(
                    innerCt => ExecuteActorOperationAsync(
                        actorId,
                    operation,
                    state,
                    innerCt),
                ct)
                : ExecuteActorOperationAsync(actorId, operation, state, ct),
            claim,
            cancellationToken,
            payloadBytes,
            metadataBytes,
            transferred);
    }

    internal ValueTask ExecuteRelocationActorAsync<TState>(
        ZLinkSpotExecutionRelocationSeal seal,
        string actorId,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        if (Volatile.Read(ref _stopping) != 0 || _isDisposed())
            return ValueTask.CompletedTask;
        var admission = TryAcquireRelocationActorAdmission(seal, actorId);
        if (admission is not { } accepted)
            return ValueTask.CompletedTask;
        var (lane, claim) = accepted;
        return RunClaimedAsync(
            lane,
            ct => _executionMode == ZLinkUserSpotExecutionMode.SpotWide
                ? _queue.RunAsync(
                    innerCt => ExecuteActorOperationAsync(
                        actorId,
                        operation,
                        state,
                        innerCt),
                    ct)
                : ExecuteActorOperationAsync(actorId, operation, state, ct),
            claim,
            cancellationToken);
    }

    internal ZLinkSpotRelocationActorQueueReservation
        ReserveRelocationActorQueue(
            ZLinkSpotExecutionRelocationSeal seal,
            string actorId)
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        if (Volatile.Read(ref _spotStopping) != 0
            || Volatile.Read(ref _stopping) != 0
            || _isDisposed())
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "SPOT relocation replay cannot reserve a stopped execution queue.");

        var (lane, claim) = AcquireRelocationActorReservationAdmission(
            seal,
            actorId);
        var reservation =
            new ZLinkSpotRelocationActorQueueReservation(actorId);
        try
        {
            // SpotWide ordering is owned by the shared queue. Reserving that
            // queue now prevents Message Follow or direct ingress from
            // overtaking a target-captured Actor frame while its mailbox turn
            // is still pending.
            var execution = RunClaimedAsync(
                    lane,
                    reservation.RunAsync,
                    claim,
                    CancellationToken.None)
                .AsTask();
            reservation.BindExecution(execution);
            return reservation;
        }
        catch
        {
            reservation.Discard();
            claim.Release();
            throw;
        }
    }

    public ValueTask ExecuteTimerAsync<TState>(
        string timerName,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(timerName);
        if (Volatile.Read(ref _spotStopping) != 0
            || Volatile.Read(ref _stopping) != 0
            || _isDisposed())
            return ValueTask.CompletedTask;
        var claim = AcquireApplicationClaim(actorLane: false);
        if (_executionMode == ZLinkUserSpotExecutionMode.SpotWide)
            return RunClaimedAsync(
                _queue,
                ct => ExecuteTimerOperationAsync(operation, state, ct),
                claim,
                cancellationToken);

        return RunClaimedAsync(
            GetLane(
                _timerLanes,
                ZLinkTimerName.FromBoundary(timerName, nameof(timerName)),
                _timerLanePolicy),
            ct => ExecuteTimerOperationAsync(operation, state, ct),
            claim,
            cancellationToken);
    }

    public bool TryRunDetached(
        string name,
        Func<CancellationToken, ValueTask> operation)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentNullException.ThrowIfNull(operation);
        return Volatile.Read(ref _stopping) == 0
               && !_isDisposed()
               && _taskRunner.TryRunDetached(name, operation);
    }

    public async ValueTask ExecuteLifecycleAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        if (Volatile.Read(ref _spotStopping) != 0 || _isDisposed()) return;

        await _queue.RunLifecycleAsync(
                _ => ExecuteLifecycleOperationAsync(operation, cancellationToken),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask ExecuteApplicationCallbackAsync<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        // A lifecycle item may need to notify application code after the
        // authority transition. Run that callback on the application lane;
        // do not execute it inline in the lifecycle lane, where Yield is not
        // a valid application operation and waiting would block the lane.
        if (ZLinkApplicationExecutionContext.Current is
                { YieldAllowed: true }
            && ZLinkSerialTurn.Current is not null)
        {
            try
            {
                await operation(_activation, state, cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                RecordApplicationWorkCompleted();
            }
            return;
        }

        var application = ExecuteAsync(operation, state, cancellationToken);
        if (ZLinkSerialTurn.Current is { } lifecycleTurn)
        {
            await lifecycleTurn.YieldFrameworkCallAsync(
                    _ => application,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await application.ConfigureAwait(false);
    }

    internal async ValueTask<bool> ExecuteQuiescentLifecycleAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask<bool>> operation,
        CancellationToken cancellationToken)
    {
        if (!TryBeginRelocationBarrier(
                holdAcceptedIngress: false,
                allowActorClaims: false,
                out var barrier))
            throw new InvalidOperationException(
                "SPOT execution lanes are already sealed.");

        try
        {
            await _queue.RunLifecycleAsync(
                    _ =>
                    {
                        MarkBarrierBoundary(barrier.Generation);
                        return ValueTask.CompletedTask;
                    },
                    cancellationToken)
                .ConfigureAwait(false);
            await barrier.Quiescent.Task
                .WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            var result = false;
            await ExecuteLifecycleAsync(
                    async (activation, ct) =>
                        result = await operation(activation, ct)
                            .ConfigureAwait(false),
                    cancellationToken)
                .ConfigureAwait(false);
            if (!result)
                AbortBarrier(barrier.Generation);
            return result;
        }
        catch
        {
            AbortBarrier(barrier.Generation);
            throw;
        }
    }

    private async ValueTask ExecuteLifecycleOperationAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        using var flow = ZLinkFlowContext.Enter(
            null,
            null,
            _flowCaptureEnabled(),
            ZLinkFlowOrigin.Lifecycle);
        await ExecuteOperationAsync(
                operation,
                null,
                cancellationToken,
                yieldAllowed: false)
            .ConfigureAwait(false);
    }

    public async ValueTask ExecuteAsync<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        if (Volatile.Read(ref _spotStopping) != 0 || _isDisposed()) return;

        var claim = AcquireApplicationClaim(actorLane: false);
        await RunClaimedAsync(
                _queue,
                ct => ExecuteOperationAsync(operation, state, ct),
                claim,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public bool Queue(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null)
        => QueueWithAdmission(
                operation,
                onSkipped,
                reportUnobservedAdmission: onSkipped is null)
            == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission QueueWithAdmission(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null,
        bool reportUnobservedAdmission = false)
    {
        var claim = TryAcquireApplicationClaim(actorLane: false);
        if (claim is null)
        {
            onSkipped?.Invoke();
            var claimAdmission = ZLinkSerialPostAdmission.Closed;
            ReportApplicationAdmissionIfUnobserved(
                "spot-application-admission",
                claimAdmission,
                reportUnobservedAdmission);
            return claimAdmission;
        }
        var admission = _queue.TryPostApplicationWithAdmission(
                async ct =>
                {
                    try
                    {
                        await ExecuteOperationAsync(operation, onSkipped, ct)
                            .ConfigureAwait(false);
                    }
                    finally
                    {
                        claim.Release();
                    }
                },
                out _);
        if (admission == ZLinkSerialPostAdmission.Accepted)
            return admission;

        claim.Release();
        onSkipped?.Invoke();
        ReportApplicationAdmissionIfUnobserved(
            "spot-application-admission",
            admission,
            reportUnobservedAdmission);
        return admission;
    }

    internal bool QueueLifecycle(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null)
    {
        if (Volatile.Read(ref _stopping) != 0 || _isDisposed())
        {
            onSkipped?.Invoke();
            return false;
        }

        var admission = _queue.TryPostNextWithAdmission(
                ct => ExecuteLifecycleOperationAsync(operation, ct),
                out _);
        if (admission == ZLinkSerialPostAdmission.Accepted)
            return true;

        onSkipped?.Invoke();
        ReportLifecycleAdmission(admission);
        return false;
    }

    internal bool QueueNext(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null)
        => QueueNextWithAdmission(
                operation,
                onSkipped,
                reportUnobservedAdmission: onSkipped is null)
            == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission QueueNextWithAdmission(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null,
        bool reportUnobservedAdmission = false)
    {
        var claim = TryAcquireApplicationClaim(actorLane: false);
        if (claim is null)
        {
            onSkipped?.Invoke();
            var claimAdmission = ZLinkSerialPostAdmission.Closed;
            ReportApplicationAdmissionIfUnobserved(
                "spot-application-admission",
                claimAdmission,
                reportUnobservedAdmission);
            return claimAdmission;
        }
        var admission = _queue.TryPostApplicationWithAdmission(
                async ct =>
                {
                    try
                    {
                        await ExecuteOperationAsync(operation, onSkipped, ct)
                            .ConfigureAwait(false);
                    }
                    finally
                    {
                        claim.Release();
                    }
                },
                out _);
        if (admission == ZLinkSerialPostAdmission.Accepted)
            return admission;

        claim.Release();
        onSkipped?.Invoke();
        ReportApplicationAdmissionIfUnobserved(
            "spot-application-admission",
            admission,
            reportUnobservedAdmission);
        return admission;
    }

    private bool IsStoppingOrDisposed() =>
        Volatile.Read(ref _stopping) != 0 || _isDisposed();

    private void ReportLifecycleAdmission(ZLinkSerialPostAdmission admission)
    {
        if (admission == ZLinkSerialPostAdmission.Closed
            && (Volatile.Read(ref _stopping) != 0 || _isDisposed()))
            return;
        _errorSink.ReportRuntimeTaskException(
            "spot-lifecycle-admission",
            new ZLinkFrameworkException(
                admission == ZLinkSerialPostAdmission.CapacityExceeded
                    ? ZLinkFrameworkErrorKind.CapacityExceeded
                    : ZLinkFrameworkErrorKind.ShuttingDown,
                admission == ZLinkSerialPostAdmission.CapacityExceeded
                    ? "The SPOT lifecycle queue capacity was exceeded."
                    : "The SPOT lifecycle queue closed before admission."));
    }

    private void ReportApplicationAdmissionIfUnobserved(
        string operation,
        ZLinkSerialPostAdmission admission,
        bool unobserved)
    {
        if (!unobserved
            || Volatile.Read(ref _stopping) != 0
            || _isDisposed())
            return;
        _errorSink.ReportRuntimeTaskException(
            operation,
            new ZLinkFrameworkException(
                admission == ZLinkSerialPostAdmission.CapacityExceeded
                    ? ZLinkFrameworkErrorKind.CapacityExceeded
                    : ZLinkFrameworkErrorKind.ShuttingDown,
                admission == ZLinkSerialPostAdmission.CapacityExceeded
                    ? "The SPOT application queue capacity was exceeded."
                    : "The SPOT application queue closed before admission."));
    }

    public ZLinkAcceptedWorkAdmission QueueAccepted(
        ReadOnlyMemory<byte> acceptedJournalRecord,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        out Task completion) =>
        QueueAccepted(
            acceptedJournalRecord,
            operation,
            relocationRelease,
            previousOwnerMessageFollow: false,
            out completion);

    public ZLinkAcceptedWorkAdmission QueueAccepted(
        ReadOnlyMemory<byte> acceptedJournalRecord,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out Task completion)
        => QueueAcceptedCore(
            acceptedJournalRecord.Length,
            null,
            acceptedJournalRecord,
            operation,
            relocationRelease,
            previousOwnerMessageFollow,
            out completion);

    internal ZLinkAcceptedWorkAdmission QueueAccepted(
        int acceptedJournalLength,
        Func<ReadOnlyMemory<byte>> acceptedJournalFactory,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out Task completion)
    {
        ArgumentNullException.ThrowIfNull(acceptedJournalFactory);
        return QueueAcceptedCore(
            acceptedJournalLength,
            acceptedJournalFactory,
            default,
            operation,
            relocationRelease,
            previousOwnerMessageFollow,
            out completion);
    }

    private ZLinkAcceptedWorkAdmission QueueAcceptedCore(
        int acceptedJournalLength,
        Func<ReadOnlyMemory<byte>>? acceptedJournalFactory,
        ReadOnlyMemory<byte> acceptedJournalRecord,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out Task completion)
    {
        var result = RunBarrierState(() =>
        {
            if (_relocationBarrier is { HoldAcceptedIngress: false })
                return (ZLinkAcceptedWorkAdmission.Closed, Task.CompletedTask);
            var callback = CreateAcceptedOperation(
                operation,
                relocationRelease);
            ZLinkAcceptedWorkAdmission admission;
            ZLinkSerialWorkItem item;
            if (acceptedJournalFactory is null)
            {
                admission = _queue.TryPostAccepted(
                    acceptedJournalRecord,
                    callback,
                    relocationRelease,
                    previousOwnerMessageFollow,
                    out item);
            }
            else
            {
                admission = _queue.TryPostAccepted(
                    acceptedJournalLength,
                    acceptedJournalFactory,
                    callback,
                    relocationRelease,
                    previousOwnerMessageFollow,
                    out item);
            }
            if (admission == ZLinkAcceptedWorkAdmission.Accepted)
                return (admission, item.Completion);
            return (admission, Task.CompletedTask);
        });
        completion = result.Item2;
        return result.Item1;
    }

    private Func<CancellationToken, ValueTask> CreateAcceptedOperation(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease) =>
        async ct =>
        {
            var claim = AcquireAdmittedApplicationClaim();
            try
            {
                await ExecuteOperationAsync(
                        operation,
                        relocationRelease,
                        ct)
                    .ConfigureAwait(false);
            }
            finally
            {
                claim.Release();
            }
        };

    internal bool TrySealRelocation(out ZLinkSpotExecutionRelocationSeal seal)
    {
        if (!TryBeginRelocationBarrier(
                holdAcceptedIngress: true,
                allowActorClaims: false,
                out var barrier))
        {
            seal = null!;
            return false;
        }
        if (!_queue.TrySealRelocation(out var queueSeal))
        {
            AbortBarrier(barrier.Generation);
            seal = null!;
            return false;
        }
        MarkBarrierBoundary(barrier.Generation);
        if (!barrier.Quiescent.Task.IsCompleted)
        {
            _queue.TryAbortRelocation(queueSeal);
            AbortBarrier(barrier.Generation);
            seal = null!;
            return false;
        }

        seal = new ZLinkSpotExecutionRelocationSeal(
            barrier.Generation,
            queueSeal);
        return true;
    }

    internal bool TrySealPerActorShellRelocation(
        out ZLinkSpotExecutionRelocationSeal seal)
    {
        if (_executionMode != ZLinkUserSpotExecutionMode.PerActor
            || !TryBeginRelocationBarrier(
                holdAcceptedIngress: true,
                allowActorClaims: true,
                out var barrier))
        {
            seal = null!;
            return false;
        }
        if (!_queue.TrySealRelocation(out var queueSeal))
        {
            AbortBarrier(barrier.Generation);
            seal = null!;
            return false;
        }
        MarkBarrierBoundary(barrier.Generation);
        if (!barrier.Quiescent.Task.IsCompleted)
        {
            _queue.TryAbortRelocation(queueSeal);
            AbortBarrier(barrier.Generation);
            seal = null!;
            return false;
        }

        seal = new ZLinkSpotExecutionRelocationSeal(
            barrier.Generation,
            queueSeal);
        return true;
    }

    internal bool TrySealRelocation(
        Func<IReadOnlyList<ZLinkAcceptedWorkRecord>, bool> admit,
        out ZLinkSpotExecutionRelocationSeal seal)
        => TrySealRelocation(0, admit, out seal, out _);

    internal bool TrySealRelocation(
        int reservedAcceptedSequences,
        Func<IReadOnlyList<ZLinkAcceptedWorkRecord>, bool> admit,
        out ZLinkSpotExecutionRelocationSeal seal,
        out ulong firstReservedSequence) =>
        TrySealRelocation(
            reservedAcceptedSequences,
            admit,
            allowActorClaims: false,
            out seal,
            out firstReservedSequence);

    internal bool TrySealPerActorShellRelocation(
        int reservedAcceptedSequences,
        Func<IReadOnlyList<ZLinkAcceptedWorkRecord>, bool> admit,
        out ZLinkSpotExecutionRelocationSeal seal,
        out ulong firstReservedSequence) =>
        TrySealRelocation(
            reservedAcceptedSequences,
            admit,
            allowActorClaims: true,
            out seal,
            out firstReservedSequence);

    private bool TrySealRelocation(
        int reservedAcceptedSequences,
        Func<IReadOnlyList<ZLinkAcceptedWorkRecord>, bool> admit,
        bool allowActorClaims,
        out ZLinkSpotExecutionRelocationSeal seal,
        out ulong firstReservedSequence)
    {
        ArgumentNullException.ThrowIfNull(admit);
        if (allowActorClaims
            && _executionMode != ZLinkUserSpotExecutionMode.PerActor)
        {
            seal = null!;
            firstReservedSequence = 0;
            return false;
        }
        if (!TryBeginRelocationBarrier(
                holdAcceptedIngress: true,
                allowActorClaims,
                out var barrier))
        {
            seal = null!;
            firstReservedSequence = 0;
            return false;
        }
        if (!_queue.TrySealRelocation(
                reservedAcceptedSequences,
                admit,
                out var queueSeal,
                out firstReservedSequence))
        {
            AbortBarrier(barrier.Generation);
            seal = null!;
            firstReservedSequence = 0;
            return false;
        }
        MarkBarrierBoundary(barrier.Generation);
        if (!barrier.Quiescent.Task.IsCompleted)
        {
            _queue.TryAbortRelocation(queueSeal);
            AbortBarrier(barrier.Generation);
            seal = null!;
            firstReservedSequence = 0;
            return false;
        }
        seal = new ZLinkSpotExecutionRelocationSeal(
            barrier.Generation,
            queueSeal);
        return true;
    }

    internal bool IsRelocationReady
        => RunBarrierState(() =>
            _relocationBarrier is null && _activeApplicationClaims == 0);

    internal bool IsPerActorShellRelocationReady
        => RunBarrierState(() =>
            _executionMode == ZLinkUserSpotExecutionMode.PerActor
            && _relocationBarrier is null
            && _activeApplicationClaims - _activeActorClaims == 0);

    // Advisory read for the idle-eviction sweep: a stale atomic read is
    // tolerated because eviction re-validates before closing, and taking
    // a state-lane turn here would contend with every message's claim/release.
    internal bool HasPendingApplicationWork =>
        Volatile.Read(ref _activeApplicationClaims) != 0;

    internal bool HasRelocationBarrier
        => RunBarrierState(() => _relocationBarrier is not null);

    internal long LastApplicationWorkCompletedAt =>
        Volatile.Read(ref _lastApplicationWorkCompletedAt);

    internal async ValueTask<ZLinkSpotExecutionRelocationSeal> SealRelocationAsync(
        CancellationToken cancellationToken)
        => await SealRelocationAsync(
                allowActorClaims: false,
                reserveAcceptedSequencesAtBoundary: static () => 0,
                cancellationToken)
            .ConfigureAwait(false);

    internal async ValueTask<ZLinkSpotExecutionRelocationSeal> SealRelocationAsync(
        bool allowActorClaims,
        Func<int> reserveAcceptedSequencesAtBoundary,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(
            reserveAcceptedSequencesAtBoundary);
        if (!TryBeginRelocationBarrier(
                holdAcceptedIngress: true,
                allowActorClaims,
                out var barrier))
            throw new InvalidOperationException(
                "SPOT execution lanes are already sealed for relocation.");

        ZLinkSerialRelocationSeal? queueSeal = null;
        try
        {
            queueSeal = await _queue
                .SealRelocationAsync(
                    reserveAcceptedSequencesAtBoundary,
                    cancellationToken)
                .ConfigureAwait(false);
            MarkBarrierBoundary(barrier.Generation);
            await barrier.Quiescent.Task
                .WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkSpotExecutionRelocationSeal(
                barrier.Generation,
                queueSeal);
        }
        catch
        {
            if (queueSeal is not null)
                _queue.TryAbortRelocation(queueSeal);
            AbortBarrier(barrier.Generation);
            throw;
        }
    }

    internal bool TryAbortRelocation(ZLinkSpotExecutionRelocationSeal seal)
    {
        ArgumentNullException.ThrowIfNull(seal);
        return RunBarrierState(() =>
        {
            if (_relocationBarrier?.Generation != seal.Generation)
                return false;
            if (!_queue.TryAbortRelocation(seal.QueueSeal))
                return false;
            _relocationAdmissionQueueOpened = false;
            _relocationBarrier = null;
            return true;
        });
    }

    internal bool TryOpenRelocationAfterMessageFollow(
        ZLinkSpotExecutionRelocationSeal seal,
        Action reserveBeforeApplicationAdmission)
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentNullException.ThrowIfNull(reserveBeforeApplicationAdmission);
        var opening = RunBarrierState(() =>
        {
            if (_relocationBarrier?.Generation != seal.Generation)
                return null;
            if (!_relocationAdmissionQueueOpened)
            {
                if (!_queue.TryOpenRelocationAfterMessageFollow(
                        seal.QueueSeal))
                    return null;
                _relocationAdmissionQueueOpened = true;
            }

            var barrier = _relocationBarrier;
            _activeApplicationClaims++;
            barrier.ActiveClaims++;
            var pending = new ZLinkRelocationAdmissionOpeningState(barrier);
            _relocationAdmissionOpening = pending;
            return pending;
        });
        if (opening is null)
            return false;

        using var scope = EnterRelocationAdmissionOpening(opening);
        try
        {
            reserveBeforeApplicationAdmission();
        }
        catch
        {
            CompleteRelocationAdmissionOpening(opening, succeeded: false);
            throw;
        }

        CompleteRelocationAdmissionOpening(opening, succeeded: true);
        return true;
    }

    internal ValueTask ExecuteSealedRelocationAsync(
        ZLinkSpotExecutionRelocationSeal seal,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentNullException.ThrowIfNull(operation);
        RunBarrierState(() =>
        {
            if (_relocationBarrier?.Generation != seal.Generation
                || !_relocationBarrier.BoundaryReached
                || !_relocationBarrier.Quiescent.Task.IsCompleted)
                throw new InvalidOperationException(
                    "SPOT relocation lifecycle requires the current quiescent seal.");
        });
        return ExecuteLifecycleOperationAsync(operation, cancellationToken);
    }

    internal bool TryCommitRelocation(
        ZLinkSpotExecutionRelocationSeal seal,
        out IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        bool preserveActorExecution = false)
    {
        ArgumentNullException.ThrowIfNull(seal);
        var result = RunBarrierState(() =>
        {
            if (_relocationBarrier?.Generation != seal.Generation)
                return (Succeeded: false,
                    Held: (IReadOnlyList<ZLinkAcceptedWorkRecord>)[]);
            if (preserveActorExecution
                && (_executionMode != ZLinkUserSpotExecutionMode.PerActor
                    || !_relocationBarrier.AllowActorClaims))
                return (Succeeded: false,
                    Held: (IReadOnlyList<ZLinkAcceptedWorkRecord>)[]);
            if (!_queue.TryCommitRelocation(seal.QueueSeal, out var committed))
                return (Succeeded: false,
                    Held: (IReadOnlyList<ZLinkAcceptedWorkRecord>)[]);
            _relocationAdmissionQueueOpened = false;
            _relocationBarrier = null;
            Interlocked.Exchange(ref _spotStopping, 1);
            if (!preserveActorExecution)
                Interlocked.Exchange(ref _stopping, 1);
            return (Succeeded: true, Held: committed);
        });
        held = result.Held;
        return result.Succeeded;
    }

    internal bool TryFreezeRelocationIngress(
        ZLinkSpotExecutionRelocationSeal seal,
        out IReadOnlyList<ZLinkAcceptedWorkRecord> held)
    {
        ArgumentNullException.ThrowIfNull(seal);
        var result = RunBarrierState(() =>
        {
            if (_relocationBarrier?.Generation != seal.Generation)
                return (Succeeded: false,
                    Held: (IReadOnlyList<ZLinkAcceptedWorkRecord>)[]);
            var succeeded = _queue.TryFreezeRelocationIngress(
                seal.QueueSeal,
                out var frozen);
            return (Succeeded: succeeded, Held: frozen);
        });
        held = result.Held;
        return result.Succeeded;
    }

    private async ValueTask ExecuteOperationAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped,
        CancellationToken cancellationToken)
    {
        if (_isDisposed())
        {
            onSkipped?.Invoke();
            return;
        }

        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(
            actorId: null,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide);
        try
        {
            await operation(_activation, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            RecordApplicationWorkCompleted();
        }
    }

    private async ValueTask ExecuteOperationAsync<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        if (_isDisposed()) return;

        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(
            actorId: null,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide);
        try
        {
            await operation(_activation, state, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            RecordApplicationWorkCompleted();
        }
    }

    private async ValueTask ExecuteOperationAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped,
        CancellationToken cancellationToken,
        bool yieldAllowed)
    {
        if (_isDisposed())
        {
            onSkipped?.Invoke();
            return;
        }

        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(actorId: null, yieldAllowed);
        await operation(_activation, cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask ExecuteActorOperationAsync<TState>(
        string actorId,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        if (_isDisposed()) return;
        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(
            actorId,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide);
        try
        {
            await operation(_activation, state, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            RecordApplicationWorkCompleted();
        }
    }

    private async ValueTask ExecuteTimerOperationAsync<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        if (_isDisposed()) return;
        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(
            actorId: null,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide);
        try
        {
            await operation(_activation, state, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            RecordApplicationWorkCompleted();
        }
    }

    private void RecordApplicationWorkCompleted() =>
        Volatile.Write(ref _lastApplicationWorkCompletedAt, Stopwatch.GetTimestamp());

    private IDisposable PushExecutionScope(string? actorId, bool yieldAllowed)
    {
        return ZLinkApplicationExecutionContext.Push(
            new ZLinkApplicationExecutionScope(
                _activation?.SpotId ?? "test-spot",
                _executionMode,
                actorId,
                yieldAllowed,
                _activation is null ? null : _activation.ContainsActor));
    }

    private T RunBarrierState<T>(
        Func<T> work,
        bool allowRelocationReservation = false)
    {
        ArgumentNullException.ThrowIfNull(work);
        while (true)
        {
            var callbackOpening = allowRelocationReservation
                                  && ReferenceEquals(
                                      CurrentRelocationAdmissionOpening.Value?.Owner,
                                      this)
                ? CurrentRelocationAdmissionOpening.Value!.Opening
                : null;
            var turn = RunState(() =>
            {
                if (_relocationAdmissionOpening is { } opening
                    && !ReferenceEquals(opening, callbackOpening))
                    return ZLinkBarrierStateTurn<T>.RetryAfter(
                        opening.Completion.Task);
                return ZLinkBarrierStateTurn<T>.FromResult(
                    RunWithoutExecutionContextFlow(work));
            });
            if (turn.Retry is null)
                return turn.Result!;
            turn.Retry.GetAwaiter().GetResult();
        }
    }

    private static T RunWithoutExecutionContextFlow<T>(Func<T> work)
    {
        if (ExecutionContext.IsFlowSuppressed())
            return work();
        using (ExecutionContext.SuppressFlow())
            return work();
    }

    private void RunBarrierState(Action work)
    {
        ArgumentNullException.ThrowIfNull(work);
        _ = RunBarrierState(() =>
        {
            work();
            return true;
        });
    }

    private T RunState<T>(Func<T> work)
    {
        _stateLane.ThrowIfReentrant();
        return _stateLane.RunAsync(work).AsTask().GetAwaiter().GetResult();
    }

    private void RunState(Action work)
    {
        _stateLane.ThrowIfReentrant();
        _stateLane.RunAsync(work).AsTask().GetAwaiter().GetResult();
    }

    private IDisposable EnterRelocationAdmissionOpening(
        ZLinkRelocationAdmissionOpeningState opening)
    {
        var previous = CurrentRelocationAdmissionOpening.Value;
        CurrentRelocationAdmissionOpening.Value =
            new ZLinkRelocationAdmissionOpeningScope(this, opening);
        return new ZLinkRelocationAdmissionOpeningScopeLease(previous);
    }

    private void CompleteRelocationAdmissionOpening(
        ZLinkRelocationAdmissionOpeningState opening,
        bool succeeded)
    {
        RunState(() =>
        {
            if (!ReferenceEquals(_relocationAdmissionOpening, opening)
                || !ReferenceEquals(_relocationBarrier, opening.Barrier))
                throw new InvalidOperationException(
                    "SPOT relocation admission opening changed before settlement.");
            if (_activeApplicationClaims <= 0
                || opening.Barrier.ActiveClaims <= 0)
                throw new InvalidOperationException(
                    "SPOT relocation admission placeholder claim is inconsistent.");

            // Reservations created by the callback have already acquired their
            // exact execution claims. Removing this one placeholder publishes
            // those claims as the complete reservation set in the same turn
            // that ordinary admission is reopened.
            _activeApplicationClaims--;
            opening.Barrier.ActiveClaims--;
            if (opening.Barrier.ActiveClaims == 0
                && opening.Barrier.BoundaryReached)
                opening.Barrier.Quiescent.TrySetResult();
            if (succeeded)
            {
                _relocationAdmissionQueueOpened = false;
                _relocationBarrier = null;
            }
            _relocationAdmissionOpening = null;
            opening.Completion.TrySetResult();
        });
    }

    private ZLinkSerialExecutionQueue GetLane<TIdentifier>(
        Dictionary<TIdentifier, ZLinkSerialExecutionQueue> lanes,
        TIdentifier key,
        ZLinkExecutionLanePolicy policy)
        where TIdentifier : notnull
    {
        _stateLane.ThrowIfReentrant();
        return _stateLane.RunAsync(() => GetLaneOnStateLane(lanes, key, policy))
            .AsTask()
            .GetAwaiter()
            .GetResult();
    }

    private ZLinkSerialExecutionQueue GetLaneOnStateLane<TIdentifier>(
        Dictionary<TIdentifier, ZLinkSerialExecutionQueue> lanes,
        TIdentifier key,
        ZLinkExecutionLanePolicy policy)
        where TIdentifier : notnull
    {
        if (lanes.TryGetValue(key, out var lane)) return lane;
        lane = CreateQueue(policy);
        lanes.Add(key, lane);
        return lane;
    }

    private void CompleteChildLanes()
    {
        _stateLane.ThrowIfReentrant();
        _ = _stateLane.RunAsync(CompleteChildLanesOnStateLane)
            .AsTask()
            .GetAwaiter()
            .GetResult();
    }

    private ZLinkSerialExecutionQueue[] CompleteChildLanesOnStateLane()
    {
        if (_completedChildLanes is not null)
            return _completedChildLanes;

        var lanes = _actorLanes.Values
            .Concat(_timerLanes.Values)
            .ToArray();
        _actorLanes.Clear();
        _timerLanes.Clear();
        foreach (var lane in lanes)
            lane.Complete();
        _completedChildLanes = lanes;
        return lanes;
    }

    private async ValueTask RunClaimedAsync(
        ZLinkSerialExecutionQueue lane,
        Func<CancellationToken, ValueTask> operation,
        ZLinkExecutionClaim claim,
        CancellationToken cancellationToken,
        long payloadBytes = 0,
        long metadataBytes = 0,
        bool transferred = false)
    {
        ZLinkSerialWorkItem item;
        try
        {
            item = await lane.PostAsync(
                    async ct =>
                    {
                        try
                        {
                            await operation(ct).ConfigureAwait(false);
                        }
                        finally
                        {
                            claim.Release();
                        }
                    },
                    payloadBytes,
                    metadataBytes,
                    transferred,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            claim.Release();
            throw;
        }

        // Caller cancellation stops only the wait. The admitted callback and a
        // yielded continuation retain the application claim until their actual
        // terminal completion, so relocation cannot capture overlapping state.
        await item.Completion.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    private ZLinkExecutionClaim AcquireApplicationClaim(bool actorLane)
    {
        return TryAcquireApplicationClaim(actorLane)
               ?? throw new ZLinkFrameworkException(
                   ZLinkFrameworkErrorKind.Rejected,
                   "SPOT application admission is sealed for relocation.");
    }

    private (ZLinkSerialExecutionQueue Lane, ZLinkExecutionClaim Claim)
        AcquireActorApplicationAdmission(string actorId)
    {
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        return RunBarrierState(() =>
        {
            if (Volatile.Read(ref _stopping) != 0
                || _relocationBarrier is { AllowActorClaims: false })
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    "SPOT application admission is sealed for relocation.");

            var lane = GetLaneOnStateLane(
                _actorLanes,
                actorKey,
                _actorLanePolicy);
            _activeApplicationClaims++;
            _activeActorClaims++;
            return (lane, new ZLinkExecutionClaim(this, actorLane: true));
        });
    }

    private ZLinkExecutionClaim? TryAcquireApplicationClaim(bool actorLane)
    {
        return RunBarrierState(() =>
        {
            if (Volatile.Read(ref _stopping) != 0
                || (!actorLane
                    && Volatile.Read(ref _spotStopping) != 0))
                return null;
            if (_relocationBarrier is { } barrier
                && (!actorLane || !barrier.AllowActorClaims))
                return null;
            _activeApplicationClaims++;
            if (actorLane) _activeActorClaims++;
            return new ZLinkExecutionClaim(this, actorLane);
        });
    }

    private ZLinkExecutionClaim AcquireAdmittedApplicationClaim()
    {
        return RunBarrierState(() =>
        {
            _activeApplicationClaims++;
            if (_relocationBarrier is { } barrier)
                barrier.ActiveClaims++;
            return new ZLinkExecutionClaim(this, actorLane: false);
        });
    }

    private (ZLinkSerialExecutionQueue Lane, ZLinkExecutionClaim Claim)?
        TryAcquireRelocationActorAdmission(
            ZLinkSpotExecutionRelocationSeal seal,
            string actorId)
    {
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        return RunBarrierState(() =>
        {
            if (Volatile.Read(ref _stopping) != 0
                || _completedChildLanes is not null)
                return null;

            var barrier = _relocationBarrier
                          ?? throw new InvalidOperationException(
                              "SPOT relocation replay requires an active relocation seal.");
            if (barrier.Generation != seal.Generation
                || !barrier.BoundaryReached
                || !barrier.Quiescent.Task.IsCompleted)
                throw new InvalidOperationException(
                    "SPOT relocation replay requires the current quiescent seal.");

            var lane = GetLaneOnStateLane(
                _actorLanes,
                actorKey,
                _actorLanePolicy);
            _activeApplicationClaims++;
            _activeActorClaims++;
            barrier.ActiveClaims++;
            return ((ZLinkSerialExecutionQueue Lane, ZLinkExecutionClaim Claim)?)
                (lane, new ZLinkExecutionClaim(this, actorLane: true));
        }, allowRelocationReservation: true);
    }

    private (ZLinkSerialExecutionQueue Lane, ZLinkExecutionClaim Claim)
        AcquireRelocationActorReservationAdmission(
            ZLinkSpotExecutionRelocationSeal seal,
            string actorId)
    {
        return RunBarrierState(() =>
        {
            if (Volatile.Read(ref _spotStopping) != 0
                || Volatile.Read(ref _stopping) != 0
                || _completedChildLanes is not null)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ShuttingDown,
                    "SPOT relocation replay cannot reserve a stopped execution queue.");

            var barrier = _relocationBarrier
                          ?? throw new InvalidOperationException(
                              "SPOT relocation replay requires an active relocation seal.");
            if (barrier.Generation != seal.Generation
                || !barrier.BoundaryReached
                || !barrier.Quiescent.Task.IsCompleted)
                throw new InvalidOperationException(
                    "SPOT relocation replay requires the current quiescent seal.");

            var lane = _executionMode == ZLinkUserSpotExecutionMode.SpotWide
                ? _queue
                : GetLaneOnStateLane(
                    _actorLanes,
                    ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
                    _actorLanePolicy);
            _activeApplicationClaims++;
            _activeActorClaims++;
            barrier.ActiveClaims++;
            return (lane, new ZLinkExecutionClaim(this, actorLane: true));
        }, allowRelocationReservation: true);
    }

    private void ReleaseApplicationClaim(bool actorLane)
    {
        RunBarrierState(() =>
        {
            if (_activeApplicationClaims <= 0)
                throw new InvalidOperationException(
                    "SPOT execution claim count is inconsistent.");
            _activeApplicationClaims--;
            if (actorLane)
            {
                if (_activeActorClaims <= 0)
                    throw new InvalidOperationException(
                        "SPOT Actor execution claim count is inconsistent.");
                _activeActorClaims--;
            }
            if (_relocationBarrier is { } barrier
                && (!actorLane || !barrier.AllowActorClaims)
                && --barrier.ActiveClaims == 0
                && barrier.BoundaryReached)
                barrier.Quiescent.TrySetResult();
        });
    }

    private bool TryBeginRelocationBarrier(
        bool holdAcceptedIngress,
        bool allowActorClaims,
        out ZLinkExecutionBarrierState barrier)
    {
        var result = RunBarrierState(() =>
        {
            if (_relocationBarrier is not null
                || _nextBarrierGeneration == ulong.MaxValue)
                return (Succeeded: false,
                    Barrier: (ZLinkExecutionBarrierState?)null);

            var created = new ZLinkExecutionBarrierState(
                _nextBarrierGeneration++,
                allowActorClaims
                    ? _activeApplicationClaims - _activeActorClaims
                    : _activeApplicationClaims,
                holdAcceptedIngress,
                allowActorClaims);
            _relocationBarrier = created;
            _relocationAdmissionQueueOpened = false;
            return (Succeeded: true, Barrier: created);
        });
        barrier = result.Barrier!;
        return result.Succeeded;
    }

    private void MarkBarrierBoundary(ulong generation)
    {
        RunBarrierState(() =>
        {
            if (_relocationBarrier is not { } barrier
                || barrier.Generation != generation)
                return;
            barrier.BoundaryReached = true;
            if (barrier.ActiveClaims == 0)
                barrier.Quiescent.TrySetResult();
        });
    }

    private void AbortBarrier(ulong generation)
    {
        RunBarrierState(() =>
        {
            if (_relocationBarrier?.Generation == generation)
            {
                _relocationAdmissionQueueOpened = false;
                _relocationBarrier = null;
            }
        });
    }

    private readonly record struct ZLinkBarrierStateTurn<T>(
        T Result,
        Task? Retry)
    {
        internal static ZLinkBarrierStateTurn<T> FromResult(T result) =>
            new(result, null);

        internal static ZLinkBarrierStateTurn<T> RetryAfter(Task retry) =>
            new(default!, retry);
    }

    private sealed class ZLinkRelocationAdmissionOpeningState(
        ZLinkExecutionBarrierState barrier)
    {
        internal ZLinkExecutionBarrierState Barrier { get; } = barrier;

        internal TaskCompletionSource Completion { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed record ZLinkRelocationAdmissionOpeningScope(
        ZLinkSpotSerialExecutor Owner,
        ZLinkRelocationAdmissionOpeningState Opening);

    private sealed class ZLinkRelocationAdmissionOpeningScopeLease(
        ZLinkRelocationAdmissionOpeningScope? previous) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                CurrentRelocationAdmissionOpening.Value = previous;
        }
    }

    private sealed class ZLinkExecutionClaim(
        ZLinkSpotSerialExecutor owner,
        bool actorLane)
    {
        private int _released;

        public void Release()
        {
            if (Interlocked.Exchange(ref _released, 1) == 0)
                owner.ReleaseApplicationClaim(actorLane);
        }
    }

    private sealed class ZLinkExecutionBarrierState(
        ulong generation,
        int activeClaims,
        bool holdAcceptedIngress,
        bool allowActorClaims)
    {
        public ulong Generation { get; } = generation;

        public int ActiveClaims { get; set; } = activeClaims;

        public bool HoldAcceptedIngress { get; } = holdAcceptedIngress;

        public bool AllowActorClaims { get; } = allowActorClaims;

        public bool BoundaryReached { get; set; }

        public TaskCompletionSource Quiescent { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }
}

internal sealed record ZLinkSpotExecutionRelocationSeal(
    ulong Generation,
    ZLinkSerialRelocationSeal QueueSeal);
