using System.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotSerialExecutor : IAsyncDisposable
{
    private const string ApplicationAdmissionOperationName = "spot-application-admission";

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
    private readonly Action? _actorConsumerSelected;
    private readonly Action<ZLinkSerialGateOperation>? _executionGateObserved;
    private ZLinkExecutionBarrierState? _relocationBarrier;
    private ZLinkRelocationAdmissionOpeningState? _relocationAdmissionOpening;
    private bool _relocationAdmissionQueueOpened;
    private ulong _nextBarrierGeneration = 1;
    private int _activeApplicationClaims;
    private int _activeActorClaims;
    private long _lastApplicationWorkCompletedAt;
    private static readonly AsyncLocal<ZLinkRelocationAdmissionOpeningScope?> CurrentRelocationAdmissionOpening =
        new();

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
        ZLinkExecutionLanePolicy? timerLanePolicy = null,
        Action? actorConsumerSelected = null,
        Action<ZLinkSerialGateOperation>? executionGateObserved = null
    )
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
        _actorConsumerSelected = actorConsumerSelected;
        _executionGateObserved = executionGateObserved;
        _executionOwner = executionOwner ?? activation?.RuntimeExecutionOwner ?? new object();
        _taskRunner = new ZLinkRuntimeTaskRunner(_errorSink, _stopToken, _executionOwner);
        _queue = CreateQueue(_spotLanePolicy);
        _lastApplicationWorkCompletedAt = Stopwatch.GetTimestamp();
    }

    private ZLinkSerialExecutionQueue CreateQueue(
        ZLinkExecutionLanePolicy policy,
        ZLinkSerialExecutionQueue? sharedOwner = null
    )
    {
        var queue = new ZLinkSerialExecutionQueue(
            _taskRunner,
            _errorSink,
            _stopToken,
            policy,
            IsApplicationStartAllowed,
            sharedGate: _executionMode == ZLinkUserSpotExecutionMode.SpotWide,
            sharedOwner: sharedOwner
        );
        queue.GateOperation = _executionGateObserved;
        return queue;
    }

    internal ZLinkSerialExecutionQueue? ActorIngressOwner =>
        _executionMode == ZLinkUserSpotExecutionMode.SpotWide ? _queue : null;

    private static bool AlwaysDisabled() => false;

    private bool IsApplicationStartAllowed(ZLinkSerialWorkItem item) =>
        Volatile.Read(ref _relocationBarrier)?.Kind != ZLinkExecutionSealKind.Close
        || item.Lane == ZLinkSerialWorkLane.Application
            && (
                !item.ReservationHeld
                || !item.IsAccepted && (_activation?.HasCommittedClose ?? true)
                || item.IsAccepted && (_activation?.HasCompletedClose ?? true)
            );

    public async ValueTask DisposeAsync()
    {
        await _queue.DisposeAsync().ConfigureAwait(false);
        var lanes = await _stateLane.RunAsync(CompleteChildLanesOnStateLane).ConfigureAwait(false);
        foreach (var lane in lanes)
            await lane.DisposeAsync().ConfigureAwait(false);
        await _stateLane.DisposeAsync().ConfigureAwait(false);
        await _taskRunner.StopAsync().ConfigureAwait(false);
    }

    public void RequestStop()
    {
        _queue.Complete();
        CompleteChildLanes();
    }

    public async ValueTask ExecuteAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken
    )
    {
        if (_isDisposed())
            return;

        var claim = AcquireApplicationClaim();
        await RunClaimedAsync(
                _queue,
                ct => ExecuteOperationAsync(operation, null, ct),
                claim,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    public ValueTask ExecuteActorAsync<TState>(
        string actorId,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken
    ) =>
        ExecuteActorAsync(
            actorId,
            operation,
            state,
            payloadBytes: 0,
            metadataBytes: 0,
            transferred: false,
            cancellationToken
        );

    internal ValueTask ExecuteActorAsync<TState>(
        string actorId,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        CancellationToken cancellationToken
    )
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        if (_isDisposed())
            return ValueTask.CompletedTask;
        var (lane, claim) = AcquireActorApplicationAdmission(actorId);
        return RunClaimedAsync(
            lane,
            ct => ExecuteActorOperationAsync(actorId, operation, state, ct),
            claim,
            cancellationToken,
            payloadBytes,
            metadataBytes,
            transferred
        );
    }

    internal ValueTask ExecuteRelocationActorAsync<TState>(
        ZLinkSpotExecutionRelocationSeal seal,
        string actorId,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        if (_isDisposed())
            return ValueTask.CompletedTask;
        var admission = TryAcquireRelocationActorAdmission(seal, actorId);
        if (admission is not { } accepted)
            return ValueTask.CompletedTask;
        var (lane, claim) = accepted;
        return RunClaimedAsync(
            lane,
            ct => ExecuteActorOperationAsync(actorId, operation, state, ct),
            claim,
            cancellationToken
        );
    }

    internal ZLinkSpotRelocationActorQueueReservation ReserveRelocationActorQueue(
        ZLinkSpotExecutionRelocationSeal seal,
        string actorId
    )
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        if (_isDisposed())
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "SPOT relocation replay cannot reserve a stopped execution queue."
            );

        var (lane, claim) = AcquireRelocationActorReservationAdmission(seal, actorId);
        var reservation = new ZLinkSpotRelocationActorQueueReservation(actorId);
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
                    CancellationToken.None,
                    ready: _executionMode == ZLinkUserSpotExecutionMode.SpotWide
                        ? () => reservation.IsReady
                        : null
                )
                .AsTask();
            reservation.BindExecution(execution, lane.NotifyReadiness);
            return reservation;
        }
        catch
        {
            reservation.Discard();
            claim.Release();
            throw;
        }
    }

    public async ValueTask ExecuteTimerAsync<TState>(
        string timerName,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken
    )
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(timerName);
        if (_isDisposed())
            return;
        var lane =
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide
                ? _queue
                : await _stateLane
                    .RunAsync(() =>
                        GetLaneOnStateLane(
                            _timerLanes,
                            ZLinkTimerName.FromBoundary(timerName, nameof(timerName)),
                            _timerLanePolicy
                        )
                    )
                    .ConfigureAwait(false);
        var claim = await AcquireApplicationClaimAsync().ConfigureAwait(false);
        await RunClaimedAsync(
                lane,
                ct => ExecuteTimerOperationAsync(operation, state, ct),
                claim,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    public bool TryRunDetached(string name, Func<CancellationToken, ValueTask> operation)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentNullException.ThrowIfNull(operation);
        return !_isDisposed() && _taskRunner.TryRunDetached(name, operation);
    }

    public async ValueTask ExecuteLifecycleAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken
    )
    {
        if (_isDisposed())
            return;

        cancellationToken.ThrowIfCancellationRequested();
        var callerFlow = ZLinkFlowContext.Current;
        var admission = PostLifecycle(
            _ => ExecuteLifecycleOperationAsync(operation, cancellationToken, callerFlow),
            out var item
        );
        if (admission != ZLinkAcceptedWorkAdmission.Accepted)
            throw new ZLinkFrameworkException(
                admission.ErrorKind(),
                "SPOT lifecycle admission is sealed."
            );
        await item.Completion.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    // Lifecycle admission is decided with the seal in one state-lane turn: a
    // Close seal (spec 06 §7 step 2) and a committed relocation end it; a
    // relocation or quiescent seal keeps it.
    private ZLinkAcceptedWorkAdmission PostLifecycle(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item
    )
    {
        var transferred = ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation();
        var result = RunBarrierState(() =>
        {
            if (_relocationBarrier is { Kind: ZLinkExecutionSealKind.Close })
                return (ZLinkAcceptedWorkAdmission.Closing, (ZLinkSerialWorkItem?)null);
            if (_queue.IsRelocated)
                return (ZLinkAcceptedWorkAdmission.RelocationMoving, null);
            var posted = _queue.TryPostNextWithAdmission(
                callback,
                payloadBytes: 0,
                metadataBytes: 0,
                transferred,
                out var postedItem
            );
            return posted == ZLinkSerialPostAdmission.Accepted
                ? (ZLinkAcceptedWorkAdmission.Accepted, postedItem)
                : (ZLinkAcceptedWorkAdmission.Closed, null);
        });
        item = result.Item2!;
        return result.Item1;
    }

    internal async ValueTask ExecuteApplicationCallbackAsync<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken
    )
    {
        // A lifecycle item may notify application code after the authority
        // transition. Transferred callbacks use the application lane.
        if (
            ZLinkApplicationExecutionContext.Current is { YieldAllowed: true }
            && ZLinkSerialTurn.Current is not null
        )
        {
            try
            {
                await operation(_activation, state, cancellationToken).ConfigureAwait(false);
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
            await lifecycleTurn
                .YieldFrameworkCallAsync(_ => application, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await application.ConfigureAwait(false);
    }

    internal async ValueTask<bool> ExecuteQuiescentLifecycleAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask<bool>> operation,
        CancellationToken cancellationToken
    )
    {
        var barrier = await TryBeginRelocationBarrierAsync(
                ZLinkExecutionSealKind.Quiescent,
                allowActorClaims: false
            )
            .ConfigureAwait(false);
        if (barrier is null)
            throw new InvalidOperationException("SPOT execution lanes are already sealed.");

        try
        {
            await _queue
                .RunLifecycleAsync(
                    _ =>
                    {
                        MarkBarrierBoundary(barrier.Generation);
                        return ValueTask.CompletedTask;
                    },
                    cancellationToken
                )
                .ConfigureAwait(false);
            await barrier.Quiescent.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            var result = false;
            await ExecuteLifecycleAsync(
                    async (activation, ct) =>
                        result = await operation(activation, ct).ConfigureAwait(false),
                    cancellationToken
                )
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

    // Posts one lifecycle operation (a Close attempt or a cold activation's
    // first-terminal record) to the lifecycle lane. Only the queue's own
    // admission decides. Null means the lane no longer admits work.
    internal Task<T>? PostLifecycleOperation<T>(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask<T>> operation
    )
    {
        var outcome = new TaskCompletionSource<T>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var admission = _queue.TryPostNextWithAdmission(
            ct =>
                ExecuteLifecycleOperationAsync(
                    async (activation, token) =>
                    {
                        try
                        {
                            outcome.TrySetResult(
                                await operation(activation, token).ConfigureAwait(false)
                            );
                        }
                        catch (Exception exception)
                        {
                            // The posting caller owns the failure; the lane item ends normally.
                            outcome.TrySetException(exception);
                        }
                    },
                    ct
                ),
            out var item
        );
        return admission == ZLinkSerialPostAdmission.Accepted ? CompleteAsync() : null;

        async Task<T> CompleteAsync()
        {
            await item.Completion.ConfigureAwait(false);
            return await outcome.Task.ConfigureAwait(false);
        }
    }

    // Spot messaging §7: the lifecycle boundary keeps unstarted message
    // records behind Close while started continuations retain their turn.
    internal async ValueTask BeginCloseBoundaryAsync(CancellationToken cancellationToken)
    {
        var barrier = await TryBeginRelocationBarrierAsync(
                ZLinkExecutionSealKind.Close,
                allowActorClaims: false
            )
            .ConfigureAwait(false);
        if (barrier is null)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "SPOT admission is already sealed by another lifecycle operation."
            );
        }
        MarkBarrierBoundary(barrier.Generation);
    }

    internal bool HasPendingAcceptedState(Func<object, bool> predicate) =>
        _queue.HasPendingAcceptedState(predicate);

    internal ValueTask<bool> HasPendingAcceptedStateOrCloseApplicationAdmissionAsync(
        Func<object, bool> predicate
    ) => _queue.HasPendingAcceptedStateOrCloseApplicationAdmissionAsync(predicate);

    internal ValueTask<bool> VisitPendingAcceptedStateAsync(
        ZLinkSpotSerialExecutor? successor,
        Action<object> visit
    ) =>
        _queue.RunOnSharedGateAsync(() =>
        {
            _queue.VisitPendingAcceptedStateOnGate(successor?._queue, visit);
            return true;
        });

    internal Task PendingApplicationCompletion => _queue.ApplicationDrained;

    internal async ValueTask AwaitStartedCloseCallsAsync(CancellationToken cancellationToken)
    {
        var barrier =
            Volatile.Read(ref _relocationBarrier)
            ?? throw new InvalidOperationException("Close boundary has not started.");
        var turn =
            ZLinkSerialTurn.Current
            ?? throw new InvalidOperationException("Close requires a lifecycle turn.");
        await turn.YieldFrameworkCallAsync(
                ct => new ValueTask(barrier.Quiescent.Task.WaitAsync(ct)),
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    internal void AbortCloseBoundary()
    {
        if (Volatile.Read(ref _relocationBarrier) is { Kind: ZLinkExecutionSealKind.Close } barrier)
        {
            AbortBarrier(barrier.Generation);
            SchedulePendingApplications();
        }
    }

    internal void SchedulePendingApplications() =>
        RunBarrierState(() =>
        {
            _queue.SchedulePendingWork();
            foreach (var lane in _actorLanes.Values)
                lane.SchedulePendingWork();
            foreach (var lane in _timerLanes.Values)
                lane.SchedulePendingWork();
        });

    private async ValueTask ExecuteLifecycleOperationAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken,
        ZLinkFlowValue? callerFlow = null
    )
    {
        // A lifecycle item that a message started (Join, leave) keeps that
        // message's flow; one without a caller flow starts a Lifecycle flow.
        using var flow = ZLinkFlowContext.Enter(
            callerFlow?.FlowId,
            callerFlow?.Origin,
            _flowCaptureEnabled(),
            ZLinkFlowOrigin.Lifecycle
        );
        await ExecuteOperationAsync(
                operation,
                null,
                cancellationToken,
                yieldAllowed: _executionMode == ZLinkUserSpotExecutionMode.SpotWide
            )
            .ConfigureAwait(false);
    }

    public async ValueTask ExecuteAsync<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken
    )
    {
        if (_isDisposed())
            return;

        var claim = AcquireApplicationClaim();
        await RunClaimedAsync(
                _queue,
                ct => ExecuteOperationAsync(operation, state, ct),
                claim,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    public bool Queue(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null
    ) =>
        QueueWithAdmission(operation, onSkipped, reportUnobservedAdmission: onSkipped is null)
        == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission QueueWithAdmission(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null,
        bool reportUnobservedAdmission = false
    ) => AwaitStateLane(QueueWithAdmissionAsync(operation, onSkipped, reportUnobservedAdmission));

    internal async ValueTask<ZLinkSerialPostAdmission> QueueWithAdmissionAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null,
        bool reportUnobservedAdmission = false
    )
    {
        var claim = await TryAcquireApplicationClaimAsync().ConfigureAwait(false);
        if (claim is null)
        {
            onSkipped?.Invoke();
            var claimAdmission = ZLinkSerialPostAdmission.Closed;
            ReportApplicationAdmissionIfUnobserved(
                ApplicationAdmissionOperationName,
                claimAdmission,
                reportUnobservedAdmission
            );
            return claimAdmission;
        }
        var admission = _queue.TryPostApplicationWithAdmission(
            async ct =>
            {
                try
                {
                    if (!TrySkipClosingApplication(onSkipped))
                        await ExecuteOperationAsync(operation, onSkipped, ct).ConfigureAwait(false);
                }
                finally
                {
                    await claim.ReleaseAsync().ConfigureAwait(false);
                }
            },
            out _
        );
        if (admission == ZLinkSerialPostAdmission.Accepted)
            return admission;

        claim.Release();
        onSkipped?.Invoke();
        ReportApplicationAdmissionIfUnobserved(
            ApplicationAdmissionOperationName,
            admission,
            reportUnobservedAdmission
        );
        return admission;
    }

    internal bool QueueLifecycle(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null
    )
    {
        if (_isDisposed())
        {
            onSkipped?.Invoke();
            return false;
        }

        var admission = PostLifecycle(ct => ExecuteLifecycleOperationAsync(operation, ct), out _);
        if (admission == ZLinkAcceptedWorkAdmission.Accepted)
            return true;

        onSkipped?.Invoke();
        if (admission == ZLinkAcceptedWorkAdmission.Closed)
            ReportLifecycleAdmission();
        return false;
    }

    internal bool QueueNext(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null
    ) =>
        QueueNextWithAdmission(operation, onSkipped, reportUnobservedAdmission: onSkipped is null)
        == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission QueueNextWithAdmission(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped = null,
        bool reportUnobservedAdmission = false
    )
    {
        var claim = TryAcquireApplicationClaim();
        if (claim is null)
        {
            onSkipped?.Invoke();
            var claimAdmission = ZLinkSerialPostAdmission.Closed;
            ReportApplicationAdmissionIfUnobserved(
                ApplicationAdmissionOperationName,
                claimAdmission,
                reportUnobservedAdmission
            );
            return claimAdmission;
        }
        var admission = _queue.TryPostApplicationWithAdmission(
            async ct =>
            {
                try
                {
                    if (!TrySkipClosingApplication(onSkipped))
                        await ExecuteOperationAsync(operation, onSkipped, ct).ConfigureAwait(false);
                }
                finally
                {
                    await claim.ReleaseAsync().ConfigureAwait(false);
                }
            },
            out _
        );
        if (admission == ZLinkSerialPostAdmission.Accepted)
            return admission;

        claim.Release();
        onSkipped?.Invoke();
        ReportApplicationAdmissionIfUnobserved(
            ApplicationAdmissionOperationName,
            admission,
            reportUnobservedAdmission
        );
        return admission;
    }

    private void ReportLifecycleAdmission()
    {
        if (_isDisposed())
            return;
        _errorSink.ReportRuntimeTaskException(
            "spot-lifecycle-admission",
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "The SPOT lifecycle queue closed before admission."
            )
        );
    }

    private void ReportApplicationAdmissionIfUnobserved(
        string operation,
        ZLinkSerialPostAdmission admission,
        bool unobserved
    )
    {
        if (!unobserved || _isDisposed())
            return;
        _errorSink.ReportRuntimeTaskException(
            operation,
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "The SPOT application queue closed before admission."
            )
        );
    }

    public ZLinkAcceptedWorkAdmission QueueAccepted(
        ReadOnlyMemory<byte> acceptedJournalRecord,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        out Task completion
    ) =>
        QueueAccepted(
            acceptedJournalRecord,
            operation,
            relocationRelease,
            previousOwnerMessageFollow: false,
            out completion
        );

    public ZLinkAcceptedWorkAdmission QueueAccepted(
        ReadOnlyMemory<byte> acceptedJournalRecord,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out Task completion
    ) =>
        QueueAcceptedCore(
            acceptedJournalRecord.Length,
            null,
            acceptedJournalRecord,
            operation,
            relocationRelease,
            previousOwnerMessageFollow,
            out completion
        );

    internal ZLinkAcceptedWorkAdmission QueueAccepted(
        int acceptedJournalLength,
        Func<ReadOnlyMemory<byte>> acceptedJournalFactory,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out Task completion,
        object? acceptedState = null
    )
    {
        ArgumentNullException.ThrowIfNull(acceptedJournalFactory);
        return QueueAcceptedCore(
            acceptedJournalLength,
            acceptedJournalFactory,
            default,
            operation,
            relocationRelease,
            previousOwnerMessageFollow,
            out completion,
            acceptedState
        );
    }

    private ZLinkAcceptedWorkAdmission QueueAcceptedCore(
        int acceptedJournalLength,
        Func<ReadOnlyMemory<byte>>? acceptedJournalFactory,
        ReadOnlyMemory<byte> acceptedJournalRecord,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out Task completion,
        object? acceptedState = null
    )
    {
        var result = RunBarrierState(() =>
            QueueAcceptedCoreOnLane(
                acceptedJournalLength,
                acceptedJournalFactory,
                acceptedJournalRecord,
                operation,
                relocationRelease,
                previousOwnerMessageFollow,
                acceptedState
            )
        );
        completion = result.Item2;
        return result.Item1;
    }

    internal async ValueTask RunIngressAsync(Action work) =>
        await RunBarrierStateAsync(() =>
            {
                work();
                return true;
            })
            .ConfigureAwait(false);

    // An ingress turn that awaits another owner's decision (the runtime inbound
    // admission) keeps the state lane for its whole duration without holding a
    // thread. A relocation opening defers it exactly like a synchronous turn.
    internal async ValueTask RunIngressAsync(Func<ValueTask> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        _stateLane.ThrowIfReentrant();
        while (true)
        {
            Task? retry = null;
            await _stateLane
                .RunAsync(() =>
                {
                    if (_relocationAdmissionOpening is { } opening)
                    {
                        retry = opening.Completion.Task;
                        return ValueTask.CompletedTask;
                    }
                    return work();
                })
                .ConfigureAwait(false);
            if (retry is null)
                return;
            await retry.ConfigureAwait(false);
        }
    }

    internal ZLinkAcceptedWorkAdmission QueueAcceptedOnLane(
        int acceptedJournalLength,
        Func<ReadOnlyMemory<byte>> acceptedJournalFactory,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out Task completion,
        object? acceptedState = null
    )
    {
        var result = QueueAcceptedCoreOnLane(
            acceptedJournalLength,
            acceptedJournalFactory,
            default,
            operation,
            relocationRelease,
            previousOwnerMessageFollow,
            acceptedState
        );
        completion = result.Item2;
        return result.Item1;
    }

    private (ZLinkAcceptedWorkAdmission, Task) QueueAcceptedCoreOnLane(
        int acceptedJournalLength,
        Func<ReadOnlyMemory<byte>>? acceptedJournalFactory,
        ReadOnlyMemory<byte> acceptedJournalRecord,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        object? acceptedState
    )
    {
        if (acceptedState is not null && _activation?.CompletedSuccessor is { } successor)
        {
            var forwarded = successor._serial.QueueAcceptedCore(
                acceptedJournalLength,
                acceptedJournalFactory,
                acceptedJournalRecord,
                operation,
                relocationRelease,
                previousOwnerMessageFollow,
                out var forwardedCompletion,
                acceptedState
            );
            return (forwarded, forwardedCompletion);
        }
        switch (_relocationBarrier?.Kind)
        {
            case ZLinkExecutionSealKind.Quiescent:
                return (ZLinkAcceptedWorkAdmission.Closed, Task.CompletedTask);
        }
        var callback = CreateAcceptedOperation(operation, relocationRelease);
        ZLinkAcceptedWorkAdmission admission;
        ZLinkSerialWorkItem item;
        if (acceptedJournalFactory is null)
        {
            admission = _queue.TryPostAccepted(
                acceptedJournalRecord,
                callback,
                relocationRelease,
                previousOwnerMessageFollow,
                out item
            );
        }
        else
        {
            admission = _queue.TryPostAccepted(
                acceptedJournalLength,
                acceptedJournalFactory,
                callback,
                relocationRelease,
                previousOwnerMessageFollow,
                out item,
                acceptedState
            );
        }
        if (admission == ZLinkAcceptedWorkAdmission.Accepted)
            return (admission, item.Completion);
        // A queue closed under the Close seal is the release decision of
        // Close step 3: this incarnation no longer admits the message.
        if (
            admission == ZLinkAcceptedWorkAdmission.Closed
            && _relocationBarrier?.Kind == ZLinkExecutionSealKind.Close
        )
            return (ZLinkAcceptedWorkAdmission.Closing, Task.CompletedTask);
        return (admission, Task.CompletedTask);
    }

    private Func<CancellationToken, ValueTask> CreateAcceptedOperation(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action relocationRelease
    ) =>
        async ct =>
        {
            var claim = await AcquireAdmittedApplicationClaimAsync().ConfigureAwait(false);
            try
            {
                await ExecuteOperationAsync(operation, relocationRelease, ct).ConfigureAwait(false);
            }
            finally
            {
                await claim.ReleaseAsync().ConfigureAwait(false);
            }
        };

    internal async ValueTask<ZLinkSpotExecutionRelocationSeal?> TrySealRelocationAsync()
    {
        var barrier = await TryBeginRelocationBarrierAsync(
                ZLinkExecutionSealKind.Relocation,
                allowActorClaims: false
            )
            .ConfigureAwait(false);
        if (barrier is null)
        {
            return null;
        }
        var queueSeal = await _queue.TrySealRelocationAsync().ConfigureAwait(false);
        if (queueSeal is null)
        {
            AbortBarrier(barrier.Generation);
            return null;
        }
        MarkBarrierBoundary(barrier.Generation);
        if (!barrier.Quiescent.Task.IsCompleted)
        {
            await _queue.TryAbortRelocationAsync(queueSeal).ConfigureAwait(false);
            AbortBarrier(barrier.Generation);
            return null;
        }

        return new ZLinkSpotExecutionRelocationSeal(barrier.Generation, queueSeal);
    }

    internal async ValueTask<ZLinkSpotExecutionRelocationSeal?> TrySealPerActorShellRelocationAsync()
    {
        if (_executionMode != ZLinkUserSpotExecutionMode.PerActor)
            return null;
        var barrier = await TryBeginRelocationBarrierAsync(
                ZLinkExecutionSealKind.Relocation,
                allowActorClaims: true
            )
            .ConfigureAwait(false);
        if (barrier is null)
        {
            return null;
        }
        var queueSeal = await _queue.TrySealRelocationAsync().ConfigureAwait(false);
        if (queueSeal is null)
        {
            AbortBarrier(barrier.Generation);
            return null;
        }
        MarkBarrierBoundary(barrier.Generation);
        if (!barrier.Quiescent.Task.IsCompleted)
        {
            await _queue.TryAbortRelocationAsync(queueSeal).ConfigureAwait(false);
            AbortBarrier(barrier.Generation);
            return null;
        }

        return new ZLinkSpotExecutionRelocationSeal(barrier.Generation, queueSeal);
    }

    internal bool IsRelocationReady =>
        RunBarrierState(() => _relocationBarrier is null && _activeApplicationClaims == 0);

    internal bool IsPerActorShellRelocationReady =>
        RunBarrierState(() =>
            _executionMode == ZLinkUserSpotExecutionMode.PerActor
            && _relocationBarrier is null
            && _activeApplicationClaims - _activeActorClaims == 0
        );

    // Advisory read for the idle-eviction sweep: a stale atomic read is
    // tolerated because eviction re-validates before closing, and taking
    // a state-lane turn here would contend with every message's claim/release.
    internal bool HasPendingApplicationWork => Volatile.Read(ref _activeApplicationClaims) != 0;

    internal bool HasRelocationBarrier => RunBarrierState(() => _relocationBarrier is not null);

    // A Close or quiescent seal is in place. Catalog lookup reads it; it does
    // not decide admission, which the queue entry decides under the seal.
    internal bool HasClosingSeal =>
        Volatile.Read(ref _relocationBarrier) is { Kind: not ZLinkExecutionSealKind.Relocation };

    internal long LastApplicationWorkCompletedAt =>
        Volatile.Read(ref _lastApplicationWorkCompletedAt);

    internal async ValueTask<ZLinkSpotExecutionRelocationSeal> SealRelocationAsync(
        CancellationToken cancellationToken
    ) =>
        await SealRelocationAsync(
                allowActorClaims: false,
                reserveAcceptedSequencesAtBoundary: static () => 0,
                cancellationToken
            )
            .ConfigureAwait(false);

    internal async ValueTask<ZLinkSpotExecutionRelocationSeal> SealRelocationAsync(
        bool allowActorClaims,
        Func<int> reserveAcceptedSequencesAtBoundary,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(reserveAcceptedSequencesAtBoundary);
        var barrier = await TryBeginRelocationBarrierAsync(
                ZLinkExecutionSealKind.Relocation,
                allowActorClaims
            )
            .ConfigureAwait(false);
        if (barrier is null)
            throw new InvalidOperationException(
                "SPOT execution lanes are already sealed for relocation."
            );

        ZLinkSerialRelocationSeal? queueSeal = null;
        try
        {
            queueSeal = await _queue
                .SealRelocationAsync(reserveAcceptedSequencesAtBoundary, cancellationToken)
                .ConfigureAwait(false);
            MarkBarrierBoundary(barrier.Generation);
            await barrier.Quiescent.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            return new ZLinkSpotExecutionRelocationSeal(barrier.Generation, queueSeal);
        }
        catch
        {
            if (queueSeal is not null)
                await _queue.TryAbortRelocationAsync(queueSeal).ConfigureAwait(false);
            AbortBarrier(barrier.Generation);
            throw;
        }
    }

    internal ValueTask<bool> TryAbortRelocationAsync(ZLinkSpotExecutionRelocationSeal seal)
    {
        ArgumentNullException.ThrowIfNull(seal);
        return RunBarrierStateAsync(
            () =>
            {
                if (_relocationBarrier?.Generation != seal.Generation)
                    return false;
                if (!_queue.TryAbortRelocationOnGate(seal.QueueSeal))
                    return false;
                _relocationAdmissionQueueOpened = false;
                _relocationBarrier = null;
                return true;
            },
            gateTurn: true
        );
    }

    internal async ValueTask<bool> TryOpenRelocationAfterMessageFollowAsync(
        ZLinkSpotExecutionRelocationSeal seal,
        Action reserveBeforeApplicationAdmission
    )
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentNullException.ThrowIfNull(reserveBeforeApplicationAdmission);
        var opening = await RunBarrierStateAsync(
                () =>
                {
                    if (_relocationBarrier?.Generation != seal.Generation)
                        return null;
                    if (!_relocationAdmissionQueueOpened)
                    {
                        if (!_queue.TryOpenRelocationAfterMessageFollowOnGate(seal.QueueSeal))
                            return null;
                        _relocationAdmissionQueueOpened = true;
                    }

                    var barrier = _relocationBarrier;
                    _activeApplicationClaims++;
                    barrier.ActiveClaims++;
                    var pending = new ZLinkRelocationAdmissionOpeningState(barrier);
                    _relocationAdmissionOpening = pending;
                    return pending;
                },
                gateTurn: true
            )
            .ConfigureAwait(false);
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

    internal async ValueTask ExecuteSealedRelocationAsync(
        ZLinkSpotExecutionRelocationSeal seal,
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentNullException.ThrowIfNull(operation);
        await RunBarrierStateAsync(() =>
            {
                if (
                    _relocationBarrier?.Generation != seal.Generation
                    || !_relocationBarrier.BoundaryReached
                    || !_relocationBarrier.Quiescent.Task.IsCompleted
                )
                    throw new InvalidOperationException(
                        "SPOT relocation lifecycle requires the current quiescent seal."
                    );
                return true;
            })
            .ConfigureAwait(false);
        await ExecuteLifecycleOperationAsync(operation, cancellationToken).ConfigureAwait(false);
    }

    internal ValueTask<(
        bool Succeeded,
        IReadOnlyList<ZLinkAcceptedWorkRecord> Held
    )> TryCommitRelocationAsync(
        ZLinkSpotExecutionRelocationSeal seal,
        bool preserveActorExecution = false
    )
    {
        ArgumentNullException.ThrowIfNull(seal);
        return RunBarrierStateAsync(
            () =>
            {
                if (_relocationBarrier?.Generation != seal.Generation)
                    return (Succeeded: false, Held: (IReadOnlyList<ZLinkAcceptedWorkRecord>)[]);
                if (
                    preserveActorExecution
                    && (
                        _executionMode != ZLinkUserSpotExecutionMode.PerActor
                        || !_relocationBarrier.AllowActorClaims
                    )
                )
                    return (Succeeded: false, Held: (IReadOnlyList<ZLinkAcceptedWorkRecord>)[]);
                if (!_queue.TryCommitRelocationOnGate(seal.QueueSeal, out var committed))
                    return (Succeeded: false, Held: (IReadOnlyList<ZLinkAcceptedWorkRecord>)[]);
                _relocationAdmissionQueueOpened = false;
                // The queue now answers every Spot-level admission as relocated.
                // Actor and timer lanes end with it unless PerActor execution stays.
                _relocationBarrier = null;
                if (!preserveActorExecution)
                    _ = CompleteChildLanesOnStateLane();
                return (Succeeded: true, Held: committed);
            },
            gateTurn: true
        );
    }

    internal ValueTask<(
        bool Succeeded,
        IReadOnlyList<ZLinkAcceptedWorkRecord> Held
    )> TryFreezeRelocationIngressAsync(ZLinkSpotExecutionRelocationSeal seal)
    {
        ArgumentNullException.ThrowIfNull(seal);
        return RunBarrierStateAsync(
            () =>
            {
                if (_relocationBarrier?.Generation != seal.Generation)
                    return (Succeeded: false, Held: (IReadOnlyList<ZLinkAcceptedWorkRecord>)[]);
                var succeeded = _queue.TryFreezeRelocationIngressOnGate(
                    seal.QueueSeal,
                    out var frozen
                );
                return (Succeeded: succeeded, Held: frozen);
            },
            gateTurn: true
        );
    }

    private async ValueTask ExecuteOperationAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        Action? onSkipped,
        CancellationToken cancellationToken
    )
    {
        if (_isDisposed())
        {
            onSkipped?.Invoke();
            return;
        }

        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(
            actorId: null,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide
        );
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
        CancellationToken cancellationToken
    )
    {
        if (_isDisposed())
            return;

        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(
            actorId: null,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide
        );
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
        bool yieldAllowed
    )
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
        CancellationToken cancellationToken
    )
    {
        if (_isDisposed())
            return;
        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(
            actorId,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide
        );
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
        CancellationToken cancellationToken
    )
    {
        if (_isDisposed())
            return;
        using var _ = ZLinkSpotAmbientContext.Push(_activation);
        using var execution = PushExecutionScope(
            actorId: null,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide
        );
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
                _activation is null ? null : _activation.ContainsActor
            )
        );
    }

    private T RunBarrierState<T>(Func<T> work, bool allowRelocationReservation = false)
    {
        ArgumentNullException.ThrowIfNull(work);
        while (true)
        {
            var callbackOpening =
                allowRelocationReservation
                && ReferenceEquals(CurrentRelocationAdmissionOpening.Value?.Owner, this)
                    ? CurrentRelocationAdmissionOpening.Value!.Opening
                    : null;
            var turn = RunState(() => RunBarrierStateTurn(work, callbackOpening));
            if (turn.Retry is null)
                return turn.Result!;
            turn.Retry.GetAwaiter().GetResult();
        }
    }

    private async ValueTask<T> RunBarrierStateAsync<T>(Func<T> work, bool gateTurn = false)
    {
        ArgumentNullException.ThrowIfNull(work);
        while (true)
        {
            var turn =
                gateTurn && _executionMode == ZLinkUserSpotExecutionMode.SpotWide
                    ? await _queue
                        .RunOnSharedGateAsync(() =>
                            RunState(() => RunBarrierStateTurn(work, callbackOpening: null))
                        )
                        .ConfigureAwait(false)
                    : await _stateLane
                        .RunAsync(() => RunBarrierStateTurn(work, callbackOpening: null))
                        .ConfigureAwait(false);
            if (turn.Retry is null)
                return turn.Result!;
            await turn.Retry.ConfigureAwait(false);
        }
    }

    private ZLinkBarrierStateTurn<T> RunBarrierStateTurn<T>(
        Func<T> work,
        ZLinkRelocationAdmissionOpeningState? callbackOpening
    )
    {
        if (
            _relocationAdmissionOpening is { } opening
            && !ReferenceEquals(opening, callbackOpening)
        )
            return ZLinkBarrierStateTurn<T>.RetryAfter(opening.Completion.Task);
        return ZLinkBarrierStateTurn<T>.FromResult(
            ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(work)
        );
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
        if (ZLinkSerialTurn.Current is { ExecutionGate: not null } turn)
        {
            var operation = work;
            work = () =>
            {
                using var scope = ZLinkSerialTurn.Push(turn);
                return operation();
            };
        }
        return AwaitStateLane(_stateLane.RunAsync(work));
    }

    private void RunState(Action work)
    {
        _stateLane.ThrowIfReentrant();
        AwaitStateLane(_stateLane.RunAsync(work));
    }

    private IDisposable EnterRelocationAdmissionOpening(
        ZLinkRelocationAdmissionOpeningState opening
    )
    {
        var previous = CurrentRelocationAdmissionOpening.Value;
        CurrentRelocationAdmissionOpening.Value = new ZLinkRelocationAdmissionOpeningScope(
            this,
            opening
        );
        return new ZLinkRelocationAdmissionOpeningScopeLease(previous);
    }

    private void CompleteRelocationAdmissionOpening(
        ZLinkRelocationAdmissionOpeningState opening,
        bool succeeded
    )
    {
        RunState(() =>
        {
            if (
                !ReferenceEquals(_relocationAdmissionOpening, opening)
                || !ReferenceEquals(_relocationBarrier, opening.Barrier)
            )
                throw new InvalidOperationException(
                    "SPOT relocation admission opening changed before settlement."
                );
            if (_activeApplicationClaims <= 0 || opening.Barrier.ActiveClaims <= 0)
                throw new InvalidOperationException(
                    "SPOT relocation admission placeholder claim is inconsistent."
                );

            // Reservations created by the callback have already acquired their
            // exact execution claims. Removing this one placeholder publishes
            // those claims as the complete reservation set in the same turn
            // that ordinary admission is reopened.
            _activeApplicationClaims--;
            opening.Barrier.ActiveClaims--;
            if (opening.Barrier.ActiveClaims == 0 && opening.Barrier.BoundaryReached)
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

    private ZLinkSerialExecutionQueue GetLaneOnStateLane<TIdentifier>(
        Dictionary<TIdentifier, ZLinkSerialExecutionQueue> lanes,
        TIdentifier key,
        ZLinkExecutionLanePolicy policy
    )
        where TIdentifier : notnull
    {
        if (lanes.TryGetValue(key, out var lane))
            return lane;
        if (_completedChildLanes is not null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "SPOT execution lanes are complete."
            );
        lane = CreateQueue(
            policy,
            _executionMode == ZLinkUserSpotExecutionMode.SpotWide ? _queue : null
        );
        if (ReferenceEquals(lanes, _actorLanes))
            lane.ConsumerSelected = _actorConsumerSelected;
        lanes.Add(key, lane);
        return lane;
    }

    private void CompleteChildLanes()
    {
        _stateLane.ThrowIfReentrant();
        _ = AwaitStateLane(_stateLane.RunAsync(CompleteChildLanesOnStateLane));
    }

    private ZLinkSerialExecutionQueue[] CompleteChildLanesOnStateLane()
    {
        if (_completedChildLanes is not null)
            return _completedChildLanes;

        var lanes = _actorLanes.Values.Concat(_timerLanes.Values).ToArray();
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
        bool transferred = false,
        Func<bool>? ready = null
    )
    {
        ZLinkSerialWorkItem item;
        try
        {
            item = await lane.PostAsync(
                    async ct =>
                    {
                        try
                        {
                            if (!TrySkipClosingApplication(null))
                                await operation(ct).ConfigureAwait(false);
                        }
                        finally
                        {
                            await claim.ReleaseAsync().ConfigureAwait(false);
                        }
                    },
                    payloadBytes,
                    metadataBytes,
                    transferred,
                    cancellationToken,
                    ready
                )
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

    private bool TrySkipClosingApplication(Action? onSkipped)
    {
        if (Volatile.Read(ref _relocationBarrier)?.Kind != ZLinkExecutionSealKind.Close)
            return false;
        onSkipped?.Invoke();
        return true;
    }

    private ZLinkExecutionClaim AcquireApplicationClaim()
    {
        return TryAcquireApplicationClaim() ?? RejectedApplicationClaim();
    }

    private async ValueTask<ZLinkExecutionClaim> AcquireApplicationClaimAsync() =>
        await TryAcquireApplicationClaimAsync().ConfigureAwait(false) ?? RejectedApplicationClaim();

    private static ZLinkExecutionClaim RejectedApplicationClaim() =>
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Rejected,
            "SPOT application admission is sealed."
        );

    private (
        ZLinkSerialExecutionQueue Lane,
        ZLinkExecutionClaim Claim
    ) AcquireActorApplicationAdmission(string actorId)
    {
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        return RunBarrierState(() =>
        {
            if (
                _completedChildLanes is not null
                || _relocationBarrier is { AllowActorClaims: false }
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    "SPOT Actor application admission is sealed."
                );

            var lane = GetLaneOnStateLane(_actorLanes, actorKey, _actorLanePolicy);
            _activeApplicationClaims++;
            _activeActorClaims++;
            return (lane, new ZLinkExecutionClaim(this, actorLane: true));
        });
    }

    // Spot-level application admission: any seal, or a committed relocation
    // of the Spot queue, refuses it.
    private ZLinkExecutionClaim? TryAcquireApplicationClaim()
    {
        return RunBarrierState(TryAcquireApplicationClaimOnLane);
    }

    private ValueTask<ZLinkExecutionClaim?> TryAcquireApplicationClaimAsync() =>
        RunBarrierStateAsync(TryAcquireApplicationClaimOnLane);

    private ZLinkExecutionClaim? TryAcquireApplicationClaimOnLane()
    {
        if (_relocationBarrier is not null || _queue.IsRelocated)
            return null;
        _activeApplicationClaims++;
        return new ZLinkExecutionClaim(this, actorLane: false);
    }

    private ValueTask<ZLinkExecutionClaim> AcquireAdmittedApplicationClaimAsync() =>
        RunBarrierStateAsync(() =>
        {
            _activeApplicationClaims++;
            if (_relocationBarrier is { } barrier)
                barrier.ActiveClaims++;
            return new ZLinkExecutionClaim(this, actorLane: false);
        });

    private (
        ZLinkSerialExecutionQueue Lane,
        ZLinkExecutionClaim Claim
    )? TryAcquireRelocationActorAdmission(ZLinkSpotExecutionRelocationSeal seal, string actorId)
    {
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        return RunBarrierState(
            () =>
            {
                if (_completedChildLanes is not null)
                    return null;

                var barrier =
                    _relocationBarrier
                    ?? throw new InvalidOperationException(
                        "SPOT relocation replay requires an active relocation seal."
                    );
                if (
                    barrier.Generation != seal.Generation
                    || !barrier.BoundaryReached
                    || !barrier.Quiescent.Task.IsCompleted
                )
                    throw new InvalidOperationException(
                        "SPOT relocation replay requires the current quiescent seal."
                    );

                var lane = GetLaneOnStateLane(_actorLanes, actorKey, _actorLanePolicy);
                _activeApplicationClaims++;
                _activeActorClaims++;
                barrier.ActiveClaims++;
                return ((ZLinkSerialExecutionQueue Lane, ZLinkExecutionClaim Claim)?)
                    (lane, new ZLinkExecutionClaim(this, actorLane: true));
            },
            allowRelocationReservation: true
        );
    }

    private (
        ZLinkSerialExecutionQueue Lane,
        ZLinkExecutionClaim Claim
    ) AcquireRelocationActorReservationAdmission(
        ZLinkSpotExecutionRelocationSeal seal,
        string actorId
    )
    {
        return RunBarrierState(
            () =>
            {
                if (_queue.IsRelocated || _completedChildLanes is not null)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.ShuttingDown,
                        "SPOT relocation replay cannot reserve a stopped execution queue."
                    );

                var barrier =
                    _relocationBarrier
                    ?? throw new InvalidOperationException(
                        "SPOT relocation replay requires an active relocation seal."
                    );
                if (
                    barrier.Generation != seal.Generation
                    || !barrier.BoundaryReached
                    || !barrier.Quiescent.Task.IsCompleted
                )
                    throw new InvalidOperationException(
                        "SPOT relocation replay requires the current quiescent seal."
                    );

                var lane =
                    _executionMode == ZLinkUserSpotExecutionMode.SpotWide
                        ? _queue
                        : GetLaneOnStateLane(
                            _actorLanes,
                            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
                            _actorLanePolicy
                        );
                _activeApplicationClaims++;
                _activeActorClaims++;
                barrier.ActiveClaims++;
                return (lane, new ZLinkExecutionClaim(this, actorLane: true));
            },
            allowRelocationReservation: true
        );
    }

    private async ValueTask ReleaseApplicationClaimAsync(bool actorLane) =>
        await RunBarrierStateAsync(() =>
            {
                ReleaseApplicationClaimOnLane(actorLane);
                return true;
            })
            .ConfigureAwait(false);

    private void ReleaseApplicationClaimOnLane(bool actorLane)
    {
        if (_activeApplicationClaims <= 0)
            throw new InvalidOperationException("SPOT execution claim count is inconsistent.");
        _activeApplicationClaims--;
        if (actorLane)
        {
            if (_activeActorClaims <= 0)
                throw new InvalidOperationException(
                    "SPOT Actor execution claim count is inconsistent."
                );
            _activeActorClaims--;
        }
        if (
            _relocationBarrier is { } barrier
            && (!actorLane || !barrier.AllowActorClaims)
            && --barrier.ActiveClaims == 0
            && barrier.BoundaryReached
        )
            barrier.Quiescent.TrySetResult();
    }

    private ValueTask<ZLinkExecutionBarrierState?> TryBeginRelocationBarrierAsync(
        ZLinkExecutionSealKind kind,
        bool allowActorClaims
    )
    {
        return RunBarrierStateAsync<ZLinkExecutionBarrierState?>(() =>
        {
            if (_relocationBarrier is not null || _nextBarrierGeneration == ulong.MaxValue)
                return null;

            var created = new ZLinkExecutionBarrierState(
                _nextBarrierGeneration++,
                allowActorClaims
                    ? _activeApplicationClaims - _activeActorClaims
                    : _activeApplicationClaims,
                kind,
                allowActorClaims
            );
            _relocationBarrier = created;
            _relocationAdmissionQueueOpened = false;
            return created;
        });
    }

    private void MarkBarrierBoundary(ulong generation)
    {
        RunState(() =>
        {
            if (_relocationBarrier is not { } barrier || barrier.Generation != generation)
                return;
            barrier.BoundaryReached = true;
            if (barrier.ActiveClaims == 0)
                barrier.Quiescent.TrySetResult();
        });
    }

    private void AbortBarrier(ulong generation)
    {
        RunState(() =>
        {
            if (_relocationBarrier?.Generation == generation)
            {
                _relocationAdmissionQueueOpened = false;
                _relocationBarrier = null;
            }
        });
    }

    private readonly record struct ZLinkBarrierStateTurn<T>(T Result, Task? Retry)
    {
        internal static ZLinkBarrierStateTurn<T> FromResult(T result) => new(result, null);

        internal static ZLinkBarrierStateTurn<T> RetryAfter(Task retry) => new(default!, retry);
    }

    private sealed class ZLinkRelocationAdmissionOpeningState(ZLinkExecutionBarrierState barrier)
    {
        internal ZLinkExecutionBarrierState Barrier { get; } = barrier;

        internal TaskCompletionSource Completion { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed record ZLinkRelocationAdmissionOpeningScope(
        ZLinkSpotSerialExecutor Owner,
        ZLinkRelocationAdmissionOpeningState Opening
    );

    private sealed class ZLinkRelocationAdmissionOpeningScopeLease(
        ZLinkRelocationAdmissionOpeningScope? previous
    ) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                CurrentRelocationAdmissionOpening.Value = previous;
        }
    }

    private sealed class ZLinkExecutionClaim(ZLinkSpotSerialExecutor owner, bool actorLane)
    {
        private int _released;

        // Blocking compatibility surface for the synchronous admission paths.
        public void Release() => AwaitStateLane(ReleaseAsync());

        public ValueTask ReleaseAsync() =>
            Interlocked.Exchange(ref _released, 1) == 0
                ? owner.ReleaseApplicationClaimAsync(actorLane)
                : ValueTask.CompletedTask;
    }

    private sealed class ZLinkExecutionBarrierState(
        ulong generation,
        int activeClaims,
        ZLinkExecutionSealKind kind,
        bool allowActorClaims
    )
    {
        public ulong Generation { get; } = generation;

        public int ActiveClaims { get; set; } = activeClaims;

        // The one field that tells the seals apart: what new ingress gets.
        public ZLinkExecutionSealKind Kind { get; } = kind;

        public bool AllowActorClaims { get; } = allowActorClaims;

        public bool BoundaryReached { get; set; }

        public TaskCompletionSource Quiescent { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }
}

internal sealed record ZLinkSpotExecutionRelocationSeal(
    ulong Generation,
    ZLinkSerialRelocationSeal QueueSeal
);

// What a sealed Spot execution queue gives new ingress (spec 06 §9): a
// relocation seal holds it, a Close seal answers Closing, and the quiescent
// seal of operational cleanup answers Closed.
internal enum ZLinkExecutionSealKind
{
    Relocation = 0,
    Close = 1,
    Quiescent = 2,
}
