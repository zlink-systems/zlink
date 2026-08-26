using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Dispatch;

internal readonly record struct ZLinkApplicationJobQueueCapacity(
    ZLinkApplicationJobQueueProfile ConfiguredProfile,
    ulong? ConfiguredManualMax,
    ulong EffectiveProcessorCount,
    ulong EffectiveMaxQueuedApplicationJobs,
    uint ConfiguredPauseThresholdPercent = 80,
    uint ConfiguredResumeThresholdPercent = 60)
{
    internal ulong PausePermitCount =>
        ZLinkApplicationJobQueueCapacityResolver.ResolvePausePermitCount(
            EffectiveMaxQueuedApplicationJobs,
            ConfiguredPauseThresholdPercent);

    internal ulong ResumePermitCount =>
        ZLinkApplicationJobQueueCapacityResolver.ResolveResumePermitCount(
            EffectiveMaxQueuedApplicationJobs,
            ConfiguredResumeThresholdPercent);
}

internal static class ZLinkApplicationJobQueueCapacityResolver
{
    private const ulong MaximumQueueLimit = int.MaxValue;

    internal static ulong ResolveEffectiveProcessorCount(
        int runtimeProcessorCount,
        int? executorMaximum = null)
    {
        var runtime = runtimeProcessorCount > 0
            ? checked((ulong)runtimeProcessorCount)
            : 1UL;
        if (executorMaximum is > 0)
            runtime = Math.Min(runtime, checked((ulong)executorMaximum.Value));
        return Math.Max(1UL, runtime);
    }

    internal static ZLinkApplicationJobQueueCapacity Resolve(
        ZLinkApplicationJobQueueProfile profile,
        ulong? configuredManualMax,
        ulong effectiveProcessorCount,
        uint pauseThresholdPercent = 80,
        uint resumeThresholdPercent = 60)
    {
        if (!Enum.IsDefined(profile))
            throw new ZLinkConfigurationException(
                $"Unknown ApplicationJobQueueProfile value '{(int)profile}'.");
        if (configuredManualMax is 0 or > MaximumQueueLimit)
            throw new ZLinkConfigurationException(
                $"MaxQueuedApplicationJobs must be between 1 and {MaximumQueueLimit}.");
        ValidatePressureThresholds(
            pauseThresholdPercent,
            resumeThresholdPercent);

        var processors = Math.Max(1UL, effectiveProcessorCount);
        var coefficient = profile switch
        {
            ZLinkApplicationJobQueueProfile.Compact => 32UL,
            ZLinkApplicationJobQueueProfile.LowLatency => 64UL,
            ZLinkApplicationJobQueueProfile.Balanced => 128UL,
            ZLinkApplicationJobQueueProfile.Throughput => 256UL,
            _ => throw new ZLinkConfigurationException(
                $"Unknown ApplicationJobQueueProfile value '{(int)profile}'.")
        };
        ulong automatic;
        try
        {
            automatic = checked(processors * coefficient);
        }
        catch (OverflowException)
        {
            throw new ZLinkConfigurationException(
                "The automatic Application Job Queue limit exceeds the supported range.");
        }
        if (automatic > MaximumQueueLimit)
            throw new ZLinkConfigurationException(
                "The automatic Application Job Queue limit exceeds 2,147,483,647.");

        return new ZLinkApplicationJobQueueCapacity(
            profile,
            configuredManualMax,
            processors,
            configuredManualMax ?? automatic,
            pauseThresholdPercent,
            resumeThresholdPercent);
    }

    internal static ulong ResolvePausePermitCount(
        ulong effectiveMaximum,
        uint pauseThresholdPercent)
    {
        if (pauseThresholdPercent is 0 or > 100)
            throw new ZLinkConfigurationException(
                "ApplicationJobQueuePauseThresholdPercent must be between 1 and 100.");
        return checked(
            (effectiveMaximum * pauseThresholdPercent + 99UL) / 100UL);
    }

    internal static ulong ResolveResumePermitCount(
        ulong effectiveMaximum,
        uint resumeThresholdPercent)
    {
        if (resumeThresholdPercent > 99)
            throw new ZLinkConfigurationException(
                "ApplicationJobQueueResumeThresholdPercent must be between 0 and 99.");
        return checked(effectiveMaximum * resumeThresholdPercent / 100UL);
    }

    internal static void ValidatePressureThresholds(
        uint pauseThresholdPercent,
        uint resumeThresholdPercent)
    {
        _ = ResolvePausePermitCount(1, pauseThresholdPercent);
        _ = ResolveResumePermitCount(1, resumeThresholdPercent);
        if (resumeThresholdPercent >= pauseThresholdPercent)
            throw new ZLinkConfigurationException(
                "ApplicationJobQueueResumeThresholdPercent must be less than "
                + "ApplicationJobQueuePauseThresholdPercent.");
    }
}

internal readonly record struct ZLinkApplicationJobQueuePressureMetrics(
    ZLinkApplicationJobQueuePressureState State,
    ulong PausedTransitionCount,
    ulong RunningTransitionCount,
    TimeSpan CurrentPauseDuration,
    TimeSpan CumulativePauseDuration,
    ulong FlowStateConfigFailures);

/// <summary>
/// Owns the host-wide FIFO permit lifecycle. A permit accounts only a reserved
/// supply record or a queued application job; running user code is outside it.
/// </summary>
internal sealed class ZLinkApplicationJobQueue : IDisposable
{
    private readonly ZLinkStateLane _lane = new();
    private readonly TimeProvider _timeProvider;
    private readonly LinkedList<Waiter> _waiters = new();
    private readonly ZLinkApplicationJobQueueCapacity _capacity;
    private readonly ZLinkReceiveFlowController _receiveFlowController;
    private ulong _reservedSupplyPermits;
    private ulong _queuedApplicationJobs;
    private ulong _peakPermitsInUse;
    private ulong _capacityWaiters;
    private ulong _capacityWaitCount;
    private TimeSpan _capacityWaitDuration;
    private ulong _measurementEpoch;
    private ZLinkApplicationJobQueuePressureState _pressureState;
    private ulong _pressureTransitionSequence;
    private ulong _pausedTransitionCount;
    private ulong _runningTransitionCount;
    private long _pauseStartedTimestamp;
    private long _cumulativePauseStartedTimestamp;
    private TimeSpan _cumulativePauseDuration;
    private bool _disposed;

    internal ZLinkApplicationJobQueue(
        ZLinkApplicationJobQueueCapacity capacity,
        TimeProvider? timeProvider = null,
        Action<Exception>? receiveFlowFailureReporter = null)
    {
        if (capacity.EffectiveMaxQueuedApplicationJobs is 0 or > int.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        ZLinkApplicationJobQueueCapacityResolver.ValidatePressureThresholds(
            capacity.ConfiguredPauseThresholdPercent,
            capacity.ConfiguredResumeThresholdPercent);
        _capacity = capacity;
        _timeProvider = timeProvider ?? TimeProvider.System;
        _receiveFlowController = new ZLinkReceiveFlowController(
            receiveFlowFailureReporter);
    }

    internal ValueTask<ZLinkApplicationJobQueueLease> AcquireAsync(
        CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
            return ValueTask.FromCanceled<ZLinkApplicationJobQueueLease>(
                cancellationToken);

        var acquired = AwaitStateLane(_lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_waiters.Count == 0
                && PermitsInUseUnderLock()
                < _capacity.EffectiveMaxQueuedApplicationJobs)
            {
                _reservedSupplyPermits = checked(_reservedSupplyPermits + 1);
                ObservePeakUnderLock();
                var immediateLease = new ZLinkApplicationJobQueueLease(this);
                return new AcquireResult(null, immediateLease, UpdatePressureStateOnLane());
            }
            var waiter = new Waiter(
                cancellationToken,
                _timeProvider.GetTimestamp(),
                _measurementEpoch);
            waiter.Node = _waiters.AddLast(waiter);
            _capacityWaiters = checked(_capacityWaiters + 1);
            return new AcquireResult(waiter, null, false);
        }));

        if (acquired.PressureChanged)
            _receiveFlowController.ApplyPending();
        if (acquired.ImmediateLease is not null)
            return ValueTask.FromResult(acquired.ImmediateLease);

        var waiter = acquired.Waiter!;

        if (cancellationToken.CanBeCanceled)
        {
            var registration = cancellationToken.UnsafeRegister(
                static state =>
                {
                    var registrationState = (CancellationRegistrationState)state!;
                    registrationState.Owner.CancelWaiter(
                        registrationState.Waiter);
                },
                new CancellationRegistrationState(this, waiter!));
            AwaitStateLane(_lane.RunAsync(() =>
            {
                waiter.CancellationRegistration = registration;
                if (waiter.State != WaiterState.Waiting)
                    registration.Dispose();
            }));
        }

        return new ValueTask<ZLinkApplicationJobQueueLease>(waiter.Completion.Task);
    }

    internal ZLinkApplicationJobQueueStatus GetStatus()
    {
        return AwaitStateLane(_lane.RunAsync(GetStatusOnLane));
    }

    private ZLinkApplicationJobQueueStatus GetStatusOnLane()
    {
        return new ZLinkApplicationJobQueueStatus(
            _capacity.ConfiguredProfile,
            _capacity.ConfiguredManualMax,
            _capacity.ConfiguredPauseThresholdPercent,
            _capacity.ConfiguredResumeThresholdPercent,
            _capacity.EffectiveProcessorCount,
            _capacity.EffectiveMaxQueuedApplicationJobs,
            _capacity.PausePermitCount,
            _capacity.ResumePermitCount,
            _reservedSupplyPermits,
            _queuedApplicationJobs,
            PermitsInUseUnderLock(),
            _peakPermitsInUse,
            _capacityWaiters,
            _capacityWaitCount,
            _capacityWaitDuration,
            _pressureState,
            CurrentPauseDurationUnderLock());
    }

    internal ZLinkApplicationJobQueuePressureMetrics GetPressureMetrics()
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            return new ZLinkApplicationJobQueuePressureMetrics(
                _pressureState,
                _pausedTransitionCount,
                _runningTransitionCount,
                CurrentPauseDurationUnderLock(),
                CumulativePauseDurationUnderLock(),
                _receiveFlowController.FlowStateConfigFailures);
        }));
    }

    internal void ResetMetrics()
    {
        AwaitStateLane(_lane.RunAsync(ResetMetricsOnLane));
    }

    private void ResetMetricsOnLane()
    {
        _peakPermitsInUse = PermitsInUseUnderLock();
        _measurementEpoch = unchecked(_measurementEpoch + 1);
        _capacityWaitCount = 0;
        _capacityWaitDuration = TimeSpan.Zero;
        _pausedTransitionCount = 0;
        _runningTransitionCount = 0;
        _cumulativePauseDuration = TimeSpan.Zero;
        _receiveFlowController.ResetMetrics();
        if (_pressureState == ZLinkApplicationJobQueuePressureState.Paused)
            _cumulativePauseStartedTimestamp = _timeProvider.GetTimestamp();
    }

    internal void MarkQueued(ZLinkApplicationJobQueueLease lease)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!lease.TryMarkQueued())
                return;
            _reservedSupplyPermits = checked(_reservedSupplyPermits - 1);
            _queuedApplicationJobs = checked(_queuedApplicationJobs + 1);
        }));
    }

    internal void Release(ZLinkApplicationJobQueueLease lease)
    {
        var released = AwaitStateLane(_lane.RunAsync(() =>
        {
            var previous = lease.TryRelease();
            if (previous == ZLinkApplicationJobQueueLease.LeaseState.Released)
                return default(ReleaseResult);
            if (previous == ZLinkApplicationJobQueueLease.LeaseState.Reserved)
                _reservedSupplyPermits = checked(_reservedSupplyPermits - 1);
            else
                _queuedApplicationJobs = checked(_queuedApplicationJobs - 1);

            while (_waiters.First is { } node)
            {
                var candidate = node.Value;
                _waiters.RemoveFirst();
                candidate.Node = null;
                if (candidate.State != WaiterState.Waiting)
                    continue;
                candidate.State = WaiterState.Admitted;
                _capacityWaiters = checked(_capacityWaiters - 1);
                RecordCompletedWaitUnderLock(candidate);
                _reservedSupplyPermits = checked(_reservedSupplyPermits + 1);
                ObservePeakUnderLock();
                return new ReleaseResult(
                    candidate,
                    new ZLinkApplicationJobQueueLease(this),
                    UpdatePressureStateOnLane());
            }
            return new ReleaseResult(null, null, UpdatePressureStateOnLane());
        }));

        if (released.PressureChanged)
            _receiveFlowController.ApplyPending();
        if (released.AdmittedWaiter is not null)
        {
            released.AdmittedWaiter.CancellationRegistration.Dispose();
            released.AdmittedWaiter.Completion.TrySetResult(released.AdmittedLease!);
        }
    }

    private void CancelWaiter(Waiter waiter)
    {
        var cancelled = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (waiter.State != WaiterState.Waiting)
                return false;
            waiter.State = WaiterState.Cancelled;
            if (waiter.Node is { } node)
            {
                _waiters.Remove(node);
                waiter.Node = null;
            }
            _capacityWaiters = checked(_capacityWaiters - 1);
            RecordCompletedWaitUnderLock(waiter);
            return true;
        }));
        if (cancelled)
            waiter.Completion.TrySetCanceled(waiter.CancellationToken);
    }

    private void RecordCompletedWaitUnderLock(Waiter waiter)
    {
        if (waiter.MeasurementEpoch != _measurementEpoch)
            return;
        _capacityWaitCount = checked(_capacityWaitCount + 1);
        var elapsed = _timeProvider.GetElapsedTime(
            waiter.StartedTimestamp,
            _timeProvider.GetTimestamp());
        if (elapsed <= TimeSpan.Zero)
            return;
        try
        {
            _capacityWaitDuration += elapsed;
        }
        catch (OverflowException)
        {
            _capacityWaitDuration = TimeSpan.MaxValue;
        }
    }

    private ulong PermitsInUseUnderLock() =>
        checked(_reservedSupplyPermits + _queuedApplicationJobs);

    private void ObservePeakUnderLock()
    {
        _peakPermitsInUse = Math.Max(
            _peakPermitsInUse,
            PermitsInUseUnderLock());
    }

    internal IDisposable RegisterReceiveFlowSocket(ISocket socket)
    {
        ArgumentNullException.ThrowIfNull(socket);
        return RegisterReceiveFlowSocket(socket, socket.SetReceiveFlowState);
    }

    internal IDisposable RegisterReceiveFlowSocket(
        object identity,
        Action<ReceiveFlowState> apply)
    {
        ArgumentNullException.ThrowIfNull(identity);
        ArgumentNullException.ThrowIfNull(apply);
        var registration = AwaitStateLane(_lane.RunAsync(
            () => _receiveFlowController.Register(identity, apply)));
        return _receiveFlowController.ApplyRegistration(identity, registration);
    }

    private bool UpdatePressureStateOnLane()
    {
        var permits = PermitsInUseUnderLock();
        var next = _pressureState;
        if (_pressureState == ZLinkApplicationJobQueuePressureState.Running
            && permits >= _capacity.PausePermitCount)
            next = ZLinkApplicationJobQueuePressureState.Paused;
        else if (_pressureState == ZLinkApplicationJobQueuePressureState.Paused
                 && permits <= _capacity.ResumePermitCount)
            next = ZLinkApplicationJobQueuePressureState.Running;
        if (next == _pressureState)
            return false;

        var now = _timeProvider.GetTimestamp();
        _pressureState = next;
        _pressureTransitionSequence = checked(_pressureTransitionSequence + 1);
        if (next == ZLinkApplicationJobQueuePressureState.Paused)
        {
            _pausedTransitionCount = checked(_pausedTransitionCount + 1);
            _pauseStartedTimestamp = now;
            _cumulativePauseStartedTimestamp = now;
        }
        else
        {
            _runningTransitionCount = checked(_runningTransitionCount + 1);
            AddCumulativePauseUnderLock(ElapsedSince(_cumulativePauseStartedTimestamp, now));
            _pauseStartedTimestamp = 0;
            _cumulativePauseStartedTimestamp = 0;
        }
        _receiveFlowController.Transition(
            next,
            _pressureTransitionSequence);
        return true;
    }

    private TimeSpan CurrentPauseDurationUnderLock() =>
        _pressureState == ZLinkApplicationJobQueuePressureState.Paused
            ? ElapsedSince(
                _pauseStartedTimestamp,
                _timeProvider.GetTimestamp())
            : TimeSpan.Zero;

    private TimeSpan CumulativePauseDurationUnderLock()
    {
        if (_pressureState != ZLinkApplicationJobQueuePressureState.Paused)
            return _cumulativePauseDuration;
        return AddSaturated(
            _cumulativePauseDuration,
            ElapsedSince(
                _cumulativePauseStartedTimestamp,
                _timeProvider.GetTimestamp()));
    }

    private TimeSpan ElapsedSince(long startedTimestamp, long nowTimestamp)
    {
        if (nowTimestamp <= startedTimestamp)
            return TimeSpan.Zero;
        return _timeProvider.GetElapsedTime(startedTimestamp, nowTimestamp);
    }

    private void AddCumulativePauseUnderLock(TimeSpan duration) =>
        _cumulativePauseDuration = AddSaturated(
            _cumulativePauseDuration,
            duration);

    private static TimeSpan AddSaturated(TimeSpan left, TimeSpan right)
    {
        try
        {
            return left + right;
        }
        catch (OverflowException)
        {
            return TimeSpan.MaxValue;
        }
    }

    public void Dispose()
    {
        var waiters = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposed)
                return Array.Empty<Waiter>();
            _disposed = true;
            _receiveFlowController.BeginClose();
            var waiting = _waiters.ToArray();
            _waiters.Clear();
            foreach (var waiter in waiting)
            {
                waiter.Node = null;
                if (waiter.State != WaiterState.Waiting)
                    continue;
                waiter.State = WaiterState.Cancelled;
                _capacityWaiters = checked(_capacityWaiters - 1);
                RecordCompletedWaitUnderLock(waiter);
            }
            return waiting;
        }));
        foreach (var waiter in waiters)
        {
            waiter.CancellationRegistration.Dispose();
            waiter.Completion.TrySetException(
                new ObjectDisposedException(nameof(ZLinkApplicationJobQueue)));
        }
    }

    private sealed class Waiter(
        CancellationToken cancellationToken,
        long startedTimestamp,
        ulong measurementEpoch)
    {
        internal CancellationToken CancellationToken { get; } = cancellationToken;
        internal long StartedTimestamp { get; } = startedTimestamp;
        internal ulong MeasurementEpoch { get; } = measurementEpoch;
        internal TaskCompletionSource<ZLinkApplicationJobQueueLease> Completion { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal LinkedListNode<Waiter>? Node { get; set; }
        internal CancellationTokenRegistration CancellationRegistration { get; set; }
        internal WaiterState State { get; set; }
    }

    private sealed record CancellationRegistrationState(
        ZLinkApplicationJobQueue Owner,
        Waiter Waiter);

    private enum WaiterState
    {
        Waiting = 0,
        Admitted = 1,
        Cancelled = 2
    }

    private readonly record struct AcquireResult(
        Waiter? Waiter,
        ZLinkApplicationJobQueueLease? ImmediateLease,
        bool PressureChanged);

    private readonly record struct ReleaseResult(
        Waiter? AdmittedWaiter,
        ZLinkApplicationJobQueueLease? AdmittedLease,
        bool PressureChanged);

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();
}

/// <summary>
/// Serializes absolute receive-flow updates for the host's supported paired
/// sockets. Its registry state shares the queue gate so permit transitions and
/// the desired socket state have one linearization point; binding calls run
/// only after that gate has been released.
/// </summary>
internal sealed class ZLinkReceiveFlowController
{
    private static readonly AsyncLocal<Entry?> CurrentApplyingEntry = new();
    private readonly ZLinkStateLane _lane = new();
    private readonly Action<Exception>? _failureReporter;
    private readonly Dictionary<object, Entry> _entries =
        new(ReferenceEqualityComparer.Instance);
    private ZLinkApplicationJobQueuePressureState _state;
    private ulong _sequence;
    private ulong _flowStateConfigFailures;
    private bool _closed;

    internal ZLinkReceiveFlowController(Action<Exception>? failureReporter)
    {
        _failureReporter = failureReporter;
    }

    internal ulong FlowStateConfigFailures
    {
        get => AwaitStateLane(_lane.RunAsync(() => _flowStateConfigFailures));
    }

    internal IDisposable Register(
        object identity,
        Action<ReceiveFlowState> apply)
    {
        Entry entry;
        entry = AwaitStateLane(_lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(
                _closed,
                this);
            if (_entries.TryGetValue(identity, out entry!))
            {
                entry.ReferenceCount = checked(entry.ReferenceCount + 1);
            }
            else
            {
                entry = new Entry(identity, apply);
                entry.Pending.Enqueue(new FlowUpdate(_sequence, _state));
                _entries.Add(identity, entry);
            }
            return entry;
        }));

        return new Registration(this, entry);
    }

    internal IDisposable ApplyRegistration(
        object identity,
        IDisposable registration)
    {
        var typedRegistration = (Registration)registration;
        var entry = typedRegistration.Entry;
        try
        {
            Apply(entry, rethrowUnexpected: true);
            AwaitStateLane(_lane.RunAsync(() =>
            {
                ObjectDisposedException.ThrowIf(
                    _closed
                    || entry.Removed
                    || !_entries.TryGetValue(identity, out var current)
                    || !ReferenceEquals(current, entry),
                    this);
            }));
            return typedRegistration;
        }
        catch
        {
            registration.Dispose();
            throw;
        }
    }

    internal void Transition(
        ZLinkApplicationJobQueuePressureState state,
        ulong sequence)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_closed)
                return;
            if (sequence <= _sequence)
                throw new InvalidOperationException(
                    "Receive-flow transition sequences must increase monotonically.");
            _state = state;
            _sequence = sequence;
            foreach (var entry in _entries.Values)
                if (!entry.Removed)
                    entry.Pending.Enqueue(new FlowUpdate(sequence, state));
        }));
    }

    internal void ApplyPending()
    {
        var entries = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_closed)
                return Array.Empty<Entry>();
            return _entries.Values.ToArray();
        }));
        foreach (var entry in entries)
            Apply(entry, rethrowUnexpected: false);
    }

    internal void BeginClose()
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_closed)
                return;
            _closed = true;
            foreach (var entry in _entries.Values)
            {
                entry.Removed = true;
                entry.Pending.Clear();
            }
            _entries.Clear();
        }));
    }

    internal void ResetMetrics()
    {
        AwaitStateLane(_lane.RunAsync(() => _flowStateConfigFailures = 0));
    }

    private void Apply(Entry entry, bool rethrowUnexpected)
    {
        while (true)
        {
            var preparation = AwaitStateLane(_lane.RunAsync(() => PrepareApply(entry)));
            if (!preparation.ShouldApply)
                return;

            Exception? failure = null;
            var previous = CurrentApplyingEntry.Value;
            CurrentApplyingEntry.Value = entry;
            try
            {
                entry.Apply(ToBindingState(preparation.Update.State));
            }
            catch (Exception error)
            {
                failure = error;
            }
            finally
            {
                CurrentApplyingEntry.Value = previous;
            }

            var expectedCloseRace = AwaitStateLane(_lane.RunAsync(() =>
                CompleteApply(entry, preparation.Update, failure)));

            if (failure is null || expectedCloseRace)
                continue;
            try
            {
                _failureReporter?.Invoke(failure);
            }
            catch
            {
            }
            if (rethrowUnexpected)
                System.Runtime.ExceptionServices.ExceptionDispatchInfo
                    .Capture(failure)
                    .Throw();
        }
    }

    private void Unregister(Entry entry)
    {
        var applying = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!entry.Removed)
            {
                entry.ReferenceCount--;
                if (entry.ReferenceCount > 0)
                    return entry.ApplyCompleted?.Task;
                entry.Removed = true;
                entry.Pending.Clear();
                _entries.Remove(entry.Identity);
            }
            return entry.ApplyCompleted?.Task;
        }));

        // Wait for an already-started binding call before the socket owner
        // closes the native handle.
        if (applying is not null && !ReferenceEquals(CurrentApplyingEntry.Value, entry))
            applying.GetAwaiter().GetResult();
    }

    private ApplyPreparation PrepareApply(Entry entry)
    {
        if (entry.Removed || entry.Pending.Count == 0 || entry.ApplyCompleted is not null)
            return default;
        entry.ApplyCompleted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        return new ApplyPreparation(true, entry.Pending.Peek());
    }

    private bool CompleteApply(Entry entry, FlowUpdate update, Exception? failure)
    {
        if (entry.Pending.Count > 0 && entry.Pending.Peek() == update)
            entry.Pending.Dequeue();
        var expectedCloseRace = (entry.Removed || _closed)
            && failure is ZlinkConfigException
            {
                Result: ZlinkConfigException.ErrorCode.InvalidState
            };
        if (failure is not null && !expectedCloseRace)
            _flowStateConfigFailures = checked(_flowStateConfigFailures + 1);
        entry.ApplyCompleted!.TrySetResult();
        entry.ApplyCompleted = null;
        return expectedCloseRace;
    }

    private static ReceiveFlowState ToBindingState(
        ZLinkApplicationJobQueuePressureState state) =>
        state switch
        {
            ZLinkApplicationJobQueuePressureState.Running =>
                ReceiveFlowState.Running,
            ZLinkApplicationJobQueuePressureState.Paused =>
                ReceiveFlowState.Paused,
            _ => throw new ArgumentOutOfRangeException(nameof(state))
        };

    private sealed class Entry(
        object identity,
        Action<ReceiveFlowState> apply)
    {
        internal object Identity { get; } = identity;
        internal Action<ReceiveFlowState> Apply { get; } = apply;
        internal Queue<FlowUpdate> Pending { get; } = new();
        internal int ReferenceCount { get; set; } = 1;
        internal bool Removed { get; set; }
        internal TaskCompletionSource? ApplyCompleted { get; set; }
    }

    private readonly record struct FlowUpdate(
        ulong Sequence,
        ZLinkApplicationJobQueuePressureState State);

    private readonly record struct ApplyPreparation(
        bool ShouldApply,
        FlowUpdate Update);

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private sealed class Registration : IDisposable
    {
        private readonly Entry _entry;
        private ZLinkReceiveFlowController? _owner;

        internal Registration(ZLinkReceiveFlowController owner, Entry entry)
        {
            _owner = owner;
            _entry = entry;
        }

        internal Entry Entry => _entry;

        public void Dispose() =>
            Interlocked.Exchange(ref _owner, null)?.Unregister(_entry);
    }
}

internal sealed class ZLinkApplicationJobQueueLease : IDisposable
{
    private readonly ZLinkApplicationJobQueue _owner;
    private int _state;

    internal ZLinkApplicationJobQueueLease(ZLinkApplicationJobQueue owner)
    {
        _owner = owner;
    }

    internal void MarkQueued() => _owner.MarkQueued(this);

    internal ZLinkApplicationJobQueue Owner => _owner;

    internal bool IsReleased =>
        Volatile.Read(ref _state) == (int)LeaseState.Released;

    internal void ReleaseForHandlerStart() => _owner.Release(this);

    internal bool TryMarkQueued() =>
        Interlocked.CompareExchange(
            ref _state,
            (int)LeaseState.Queued,
            (int)LeaseState.Reserved)
        == (int)LeaseState.Reserved;

    internal LeaseState TryRelease() =>
        (LeaseState)Interlocked.Exchange(ref _state, (int)LeaseState.Released);

    public void Dispose() => _owner.Release(this);

    internal enum LeaseState
    {
        Reserved = 0,
        Queued = 1,
        Released = 2
    }
}

internal sealed class ZLinkApplicationJobQueueRecordOwner : IDisposable
{
    private IDisposable? _payloadOwner;
    private ZLinkApplicationJobQueueLease? _admission;

    internal ZLinkApplicationJobQueueRecordOwner(
        IDisposable? payloadOwner,
        ZLinkApplicationJobQueueLease admission)
    {
        _payloadOwner = payloadOwner;
        _admission = admission;
    }

    internal ZLinkApplicationJobQueueLease? Admission =>
        Volatile.Read(ref _admission);

    public void Dispose()
    {
        try
        {
            Interlocked.Exchange(ref _payloadOwner, null)?.Dispose();
        }
        finally
        {
            Interlocked.Exchange(ref _admission, null)?.Dispose();
        }
    }
}

internal static class ZLinkApplicationJobQueueInvocation
{
    private static readonly AsyncLocal<Scope?> Current = new();

    internal static IDisposable Enter(ZLinkApplicationJobQueueLease lease)
    {
        ArgumentNullException.ThrowIfNull(lease);
        var scope = new Scope(Current.Value, lease);
        Current.Value = scope;
        return scope;
    }

    internal static void ReleaseForHandlerStart()
    {
        var scope = Current.Value;
        if (scope is not null)
            Interlocked.Exchange(ref scope.Lease, null)?.ReleaseForHandlerStart();
    }

    internal static async ValueTask EnsureQueuedPermitAsync(
        CancellationToken cancellationToken)
    {
        var scope = Current.Value;
        if (scope is null)
            return;

        while (true)
        {
            var current = Volatile.Read(ref scope.Lease);
            if (current is not null && !current.IsReleased)
            {
                current.MarkQueued();
                return;
            }

            var acquired = await scope.Owner.AcquireAsync(cancellationToken)
                .ConfigureAwait(false);
            if (Interlocked.CompareExchange(
                    ref scope.Lease,
                    acquired,
                    current) == current)
            {
                acquired.MarkQueued();
                return;
            }

            acquired.Dispose();
        }
    }

    private sealed class Scope(
        Scope? previous,
        ZLinkApplicationJobQueueLease lease) : IDisposable
    {
        private readonly Scope? _previous = previous;
        internal readonly ZLinkApplicationJobQueue Owner = lease.Owner;
        internal ZLinkApplicationJobQueueLease? Lease = lease;
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0)
                return;
            Interlocked.Exchange(ref Lease, null)?.Dispose();
            if (ReferenceEquals(Current.Value, this))
                Current.Value = _previous;
        }
    }
}
