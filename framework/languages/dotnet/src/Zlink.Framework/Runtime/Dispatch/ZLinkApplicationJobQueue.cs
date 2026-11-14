using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Dispatch;

internal readonly record struct ZLinkApplicationJobQueueCapacity(
    ZLinkApplicationJobQueueProfile ConfiguredProfile,
    ulong? ConfiguredManualMax,
    ulong EffectiveProcessorCount,
    ulong EffectiveMaxQueuedApplicationJobs,
    uint ConfiguredPauseThresholdPercent =
        ZLinkInboundDispatchOptionsModel.DefaultPauseThresholdPercent,
    uint ConfiguredResumeThresholdPercent =
        ZLinkInboundDispatchOptionsModel.DefaultResumeThresholdPercent
)
{
    internal ulong PausePermitCount =>
        ZLinkApplicationJobQueueCapacityResolver.ResolvePausePermitCount(
            EffectiveMaxQueuedApplicationJobs,
            ConfiguredPauseThresholdPercent
        );

    internal ulong ResumePermitCount =>
        ZLinkApplicationJobQueueCapacityResolver.ResolveResumePermitCount(
            EffectiveMaxQueuedApplicationJobs,
            ConfiguredResumeThresholdPercent
        );
}

internal static class ZLinkApplicationJobQueueCapacityResolver
{
    private const ulong MaximumQueueLimit = int.MaxValue;

    internal static ulong ResolveEffectiveProcessorCount(
        int runtimeProcessorCount,
        int? executorMaximum = null
    )
    {
        var runtime = runtimeProcessorCount > 0 ? checked((ulong)runtimeProcessorCount) : 1UL;
        if (executorMaximum is > 0)
            runtime = Math.Min(runtime, checked((ulong)executorMaximum.Value));
        return Math.Max(1UL, runtime);
    }

    internal static ZLinkApplicationJobQueueCapacity Resolve(
        ZLinkApplicationJobQueueProfile profile,
        ulong? configuredManualMax,
        ulong effectiveProcessorCount,
        uint pauseThresholdPercent = ZLinkInboundDispatchOptionsModel.DefaultPauseThresholdPercent,
        uint resumeThresholdPercent = ZLinkInboundDispatchOptionsModel.DefaultResumeThresholdPercent
    )
    {
        if (!Enum.IsDefined(profile))
            throw new ZLinkConfigurationException(
                $"Unknown ApplicationJobQueueProfile value '{(int)profile}'."
            );
        if (configuredManualMax is 0 or > MaximumQueueLimit)
            throw new ZLinkConfigurationException(
                $"MaxQueuedApplicationJobs must be positive and at most {MaximumQueueLimit}."
            );
        ValidatePressureThresholds(pauseThresholdPercent, resumeThresholdPercent);

        var processors = Math.Max(1UL, effectiveProcessorCount);
        var coefficient = profile switch
        {
            ZLinkApplicationJobQueueProfile.Compact => 32UL,
            ZLinkApplicationJobQueueProfile.LowLatency => 64UL,
            ZLinkApplicationJobQueueProfile.Balanced => 128UL,
            ZLinkApplicationJobQueueProfile.Throughput => 256UL,
            _ => throw new ZLinkConfigurationException(
                $"Unknown ApplicationJobQueueProfile value '{(int)profile}'."
            ),
        };
        ulong automatic;
        try
        {
            automatic = checked(processors * coefficient);
        }
        catch (OverflowException)
        {
            throw new ZLinkConfigurationException(
                "The automatic Application Job Queue limit exceeds the supported range."
            );
        }
        if (automatic > MaximumQueueLimit)
            throw new ZLinkConfigurationException(
                $"The automatic Application Job Queue limit exceeds {MaximumQueueLimit}."
            );

        return new ZLinkApplicationJobQueueCapacity(
            profile,
            configuredManualMax,
            processors,
            configuredManualMax ?? automatic,
            pauseThresholdPercent,
            resumeThresholdPercent
        );
    }

    internal static ulong ResolvePausePermitCount(
        ulong effectiveMaximum,
        uint pauseThresholdPercent
    )
    {
        if (pauseThresholdPercent is 0 or > 100)
            throw new ZLinkConfigurationException(
                "ApplicationJobQueuePauseThresholdPercent must be between 1 and 100."
            );
        return checked((effectiveMaximum * pauseThresholdPercent + 99UL) / 100UL);
    }

    internal static ulong ResolveResumePermitCount(
        ulong effectiveMaximum,
        uint resumeThresholdPercent
    )
    {
        if (resumeThresholdPercent > 99)
            throw new ZLinkConfigurationException(
                "ApplicationJobQueueResumeThresholdPercent must be between 0 and 99."
            );
        return checked(effectiveMaximum * resumeThresholdPercent / 100UL);
    }

    internal static void ValidatePressureThresholds(
        uint pauseThresholdPercent,
        uint resumeThresholdPercent
    )
    {
        _ = ResolvePausePermitCount(1, pauseThresholdPercent);
        _ = ResolveResumePermitCount(1, resumeThresholdPercent);
        if (resumeThresholdPercent >= pauseThresholdPercent)
            throw new ZLinkConfigurationException(
                "ApplicationJobQueueResumeThresholdPercent must be less than "
                    + "ApplicationJobQueuePauseThresholdPercent."
            );
    }
}

internal readonly record struct ZLinkApplicationJobQueuePressureMetrics(
    ZLinkApplicationJobQueuePressureState State,
    ulong PausedTransitionCount,
    ulong RunningTransitionCount,
    TimeSpan CurrentPauseDuration,
    TimeSpan CumulativePauseDuration,
    ulong FlowStateConfigFailures
);

internal enum ZLinkApplicationJobOrigin
{
    Remote,
    Local,
}

/// <summary>
/// Owns the host-wide FIFO permit lifecycle. A permit accounts only a reserved
/// supply record or a queued application job; running user code is outside it.
/// </summary>
internal sealed class ZLinkApplicationJobQueue : IDisposable
{
    private readonly object _sync = new();
    private readonly ILogger<ZLinkApplicationJobQueue>? _logger;
    private readonly Action<Exception>? _loggerFailureReporter;
    private readonly TimeProvider _timeProvider;
    private readonly LinkedList<Waiter> _waiters = new();
    private readonly ZLinkApplicationJobQueueCapacity _capacity;
    private readonly ZLinkReceiveFlowController _receiveFlowController;
    private ulong _reservedSupplyPermits;
    private ulong _queuedApplicationJobs;
    private ulong _peakPermitsInUse;
    private ulong _capacityWaiters;
    private long _localWaiters;
    private bool _localBacklogLogged;
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
        Action<Exception>? receiveFlowFailureReporter = null,
        ILogger<ZLinkApplicationJobQueue>? logger = null,
        Action<Exception>? loggerFailureReporter = null
    )
    {
        if (capacity.EffectiveMaxQueuedApplicationJobs is 0 or > int.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        ZLinkApplicationJobQueueCapacityResolver.ValidatePressureThresholds(
            capacity.ConfiguredPauseThresholdPercent,
            capacity.ConfiguredResumeThresholdPercent
        );
        _capacity = capacity;
        _timeProvider = timeProvider ?? TimeProvider.System;
        _logger = logger;
        _loggerFailureReporter = loggerFailureReporter;
        _receiveFlowController = new ZLinkReceiveFlowController(_sync, receiveFlowFailureReporter);
    }

    internal ValueTask<ZLinkApplicationJobQueueLease> AcquireAsync(
        CancellationToken cancellationToken,
        ZLinkApplicationJobOrigin origin
    )
    {
        // Queue-turn and failure-reporter exceptions keep their synchronous delivery.
        return ClaimAsync(AcquireBatchAsync(1, cancellationToken, origin));

        static async ValueTask<ZLinkApplicationJobQueueLease> ClaimAsync(
            ValueTask<ZLinkApplicationJobQueueReservation> pending
        )
        {
            var reservation = await pending.ConfigureAwait(false);
            return reservation.Claim()!;
        }
    }

    internal ValueTask<ZLinkApplicationJobQueueReservation> AcquireBatchAsync(
        int maximum,
        CancellationToken cancellationToken,
        ZLinkApplicationJobOrigin origin
    )
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(maximum);
        return AcquireAsyncCore(maximum, cancellationToken, null, origin);
    }

    internal ValueTask<ZLinkApplicationJobQueueReservation> AcquireAsyncCore(
        int maximum,
        CancellationToken cancellationToken,
        ZLinkPreparedWork<ZLinkApplicationJobQueueLease?>? publication,
        ZLinkApplicationJobOrigin origin
    )
    {
        if (cancellationToken.IsCancellationRequested)
            return ValueTask.FromCanceled<ZLinkApplicationJobQueueReservation>(cancellationToken);

        var clock = ReadClock();
        AcquireResult acquired;
        lock (_sync)
        {
            var reserved = ReserveAvailableUnderLock(maximum, clock);
            if (reserved.Count != 0)
            {
                var reservation = CreateReservation(reserved.Count, origin, publication);
                acquired = new AcquireResult(
                    null,
                    reservation.Reservation,
                    reserved.PressureChanged,
                    PublicationEnqueued: reservation.PublicationEnqueued
                );
            }
            else
            {
                var waiting = new Waiter(
                    cancellationToken,
                    clock.Now,
                    _measurementEpoch,
                    publication,
                    origin,
                    maximum
                );
                waiting.Node = _waiters.AddLast(waiting);
                _capacityWaiters = checked(_capacityWaiters + 1);
                acquired = new AcquireResult(
                    waiting,
                    default,
                    false,
                    UpdateLocalWaitersUnderLock(origin, 1)
                );
            }
        }

        if (acquired.Reservation.Count != 0)
            publication?.Schedule(acquired.PublicationEnqueued);
        if (acquired.LocalBacklogWarning is { } localWaiters)
            LogLocalBacklog(localWaiters);
        if (acquired.PressureChanged)
            _receiveFlowController.ApplyPending();
        if (acquired.Reservation.Count != 0)
            return ValueTask.FromResult(acquired.Reservation);

        var waiter = acquired.Waiter!;
        if (cancellationToken.CanBeCanceled)
        {
            var registration = cancellationToken.UnsafeRegister(
                static state =>
                {
                    var registrationState = (CancellationRegistrationState)state!;
                    registrationState.Owner.CancelWaiter(registrationState.Waiter);
                },
                new CancellationRegistrationState(this, waiter)
            );
            bool disposeRegistration;
            lock (_sync)
            {
                disposeRegistration = waiter.State != WaiterState.Waiting;
                if (!disposeRegistration)
                    waiter.CancellationRegistration = registration;
            }
            if (disposeRegistration)
                registration.Dispose();
        }
        return new ValueTask<ZLinkApplicationJobQueueReservation>(waiter.Completion.Task);
    }

    internal bool TryAcquire(
        out ZLinkApplicationJobQueueLease? lease,
        ZLinkApplicationJobOrigin origin
    )
    {
        lease = TryAcquireAsync(origin).GetAwaiter().GetResult();
        return lease is not null;
    }

    internal ValueTask<ZLinkApplicationJobQueueLease?> TryAcquireAsync(
        ZLinkApplicationJobOrigin origin
    )
    {
        try
        {
            var clock = ReadClock();
            (int Count, bool PressureChanged) acquired;
            lock (_sync)
                acquired = ReserveAvailableUnderLock(1, clock);
            if (acquired.PressureChanged)
                _receiveFlowController.ApplyPending();
            return ValueTask.FromResult(
                acquired.Count == 0 ? null : new ZLinkApplicationJobQueueLease(this, origin)
            );
        }
        catch (Exception failure)
        {
            // Preserve the asynchronous API's fault delivery without a lane turn.
            return ValueTask.FromException<ZLinkApplicationJobQueueLease?>(failure);
        }
    }

    // Spec 08 I1: reserve a rotation's count before claiming records; leases
    // exist only for claimed records and the rotation returns the unused count.
    internal int TryReserveBatch(int maximum)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(maximum);
        var clock = ReadClock();
        (int Count, bool PressureChanged) acquired;
        lock (_sync)
            acquired = ReserveAvailableUnderLock(maximum, clock);
        if (acquired.PressureChanged)
            _receiveFlowController.ApplyPending();
        return acquired.Count;
    }

    private (int Count, bool PressureChanged) ReserveAvailableUnderLock(
        int maximum,
        (long Now, long Frequency) clock
    )
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_waiters.Count != 0)
            return (0, false);
        return ReserveUnderLock(maximum, clock);
    }

    private (int Count, bool PressureChanged) ReserveUnderLock(
        int maximum,
        (long Now, long Frequency) clock
    )
    {
        var count = (int)
            Math.Min(
                (ulong)maximum,
                _capacity.EffectiveMaxQueuedApplicationJobs - PermitsInUseUnderLock()
            );
        if (count == 0)
            return (0, false);
        _reservedSupplyPermits = checked(_reservedSupplyPermits + (ulong)count);
        ObservePeakUnderLock();
        return (count, UpdatePressureStateUnderLock(clock));
    }

    private (
        ZLinkApplicationJobQueueReservation Reservation,
        bool PublicationEnqueued
    ) CreateReservation(
        int count,
        ZLinkApplicationJobOrigin origin,
        ZLinkPreparedWork<ZLinkApplicationJobQueueLease?>? publication
    )
    {
        ZLinkApplicationJobQueueLease? held = null;
        var enqueued = false;
        if (publication is not null)
        {
            held = new ZLinkApplicationJobQueueLease(this, origin);
            enqueued = publication.Enqueue(held);
            count--;
        }
        return (new ZLinkApplicationJobQueueReservation(this, origin, held, count), enqueued);
    }

    internal ZLinkApplicationJobQueueStatus GetStatus()
    {
        var clock = ReadClock();
        lock (_sync)
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
                CurrentPauseDurationUnderLock(clock)
            );
    }

    internal ZLinkApplicationJobQueuePressureMetrics GetPressureMetrics()
    {
        var clock = ReadClock();
        lock (_sync)
            return new ZLinkApplicationJobQueuePressureMetrics(
                _pressureState,
                _pausedTransitionCount,
                _runningTransitionCount,
                CurrentPauseDurationUnderLock(clock),
                CumulativePauseDurationUnderLock(clock),
                _receiveFlowController.FlowStateConfigFailures
            );
    }

    internal void ResetMetrics()
    {
        var clock = ReadClock();
        lock (_sync)
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
                _cumulativePauseStartedTimestamp = clock.Now;
        }
    }

    internal void MarkQueued(ZLinkApplicationJobQueueLease lease)
    {
        lock (_sync)
        {
            if (!lease.TryMarkQueued())
                return;
            _reservedSupplyPermits = checked(_reservedSupplyPermits - 1);
            _queuedApplicationJobs = checked(_queuedApplicationJobs + 1);
        }
    }

    internal void Release(ZLinkApplicationJobQueueLease lease) => _ = ReleaseAsync(lease);

    internal ValueTask ReleaseForHandlerStartAsync(ZLinkApplicationJobQueueLease lease) =>
        new(ReleaseAsync(lease));

    // State changes share the lock; scheduling, waiter completion and socket
    // notifications run after it is released.
    private Task ReleaseAsync(ZLinkApplicationJobQueueLease lease)
    {
        try
        {
            var clock = ReadClock();
            ReleaseResult released;
            lock (_sync)
                released = ReleaseUnderLock(lease, clock);
            CompleteRelease(released);
            return Task.CompletedTask;
        }
        catch (Exception failure)
        {
            return Task.FromException(failure);
        }
    }

    internal void ReleaseReserved(int count)
    {
        if (count > 0)
            _ = ReleaseReservedAsync(count);
    }

    private Task ReleaseReservedAsync(int count)
    {
        try
        {
            var clock = ReadClock();
            List<ReleaseResult>? released;
            lock (_sync)
                released = ReleaseReservedUnderLock(count, clock);
            if (released is not null)
                foreach (var result in released)
                    CompleteRelease(result);
            return Task.CompletedTask;
        }
        catch (Exception failure)
        {
            return Task.FromException(failure);
        }
    }

    private List<ReleaseResult>? ReleaseReservedUnderLock(
        int count,
        (long Now, long Frequency) clock
    )
    {
        _reservedSupplyPermits = checked(_reservedSupplyPermits - (ulong)count);
        List<ReleaseResult>? results = null;
        ReleaseResult result;
        do
        {
            result = AdmitNextWaiterUnderLock(clock);
            if (result.AdmittedWaiter is not null || result.PressureChanged)
                (results ??= []).Add(result);
        } while (result.AdmittedWaiter is not null);
        return results;
    }

    private ReleaseResult ReleaseUnderLock(
        ZLinkApplicationJobQueueLease lease,
        (long Now, long Frequency) clock
    )
    {
        var previous = lease.TryRelease();
        if (previous == ZLinkApplicationJobQueueLease.LeaseState.Released)
            return default;
        if (previous == ZLinkApplicationJobQueueLease.LeaseState.Reserved)
            _reservedSupplyPermits = checked(_reservedSupplyPermits - 1);
        else
            _queuedApplicationJobs = checked(_queuedApplicationJobs - 1);
        return AdmitNextWaiterUnderLock(clock);
    }

    private ReleaseResult AdmitNextWaiterUnderLock((long Now, long Frequency) clock)
    {
        if (_waiters.First is { } node)
        {
            var candidate = node.Value;
            var reserved = ReserveUnderLock(candidate.Maximum, clock);
            if (reserved.Count != 0)
            {
                _waiters.RemoveFirst();
                candidate.Node = null;
                candidate.State = WaiterState.Admitted;
                _capacityWaiters = checked(_capacityWaiters - 1);
                RecordCompletedWaitUnderLock(candidate, clock);
                var reservation = CreateReservation(
                    reserved.Count,
                    candidate.Origin,
                    candidate.Publication
                );
                return new ReleaseResult(
                    candidate,
                    reservation.Reservation,
                    reserved.PressureChanged,
                    candidate.CancellationRegistration,
                    reservation.PublicationEnqueued
                );
            }
        }
        return new ReleaseResult(null, default, UpdatePressureStateUnderLock(clock));
    }

    private void CompleteRelease(ReleaseResult released)
    {
        released.AdmittedWaiter?.Publication?.Schedule(released.PublicationEnqueued);
        if (released.PressureChanged)
            _receiveFlowController.ApplyPending();
        if (released.AdmittedWaiter is not null)
        {
            released.CancellationRegistration.Dispose();
            released.AdmittedWaiter.Completion.TrySetResult(released.Reservation);
        }
    }

    private void CancelWaiter(Waiter waiter)
    {
        var clock = ReadClock();
        lock (_sync)
        {
            if (waiter.State != WaiterState.Waiting)
                return;
            waiter.State = WaiterState.Cancelled;
            if (waiter.Node is { } node)
            {
                _waiters.Remove(node);
                waiter.Node = null;
            }
            _capacityWaiters = checked(_capacityWaiters - 1);
            RecordCompletedWaitUnderLock(waiter, clock);
        }
        waiter.Completion.TrySetCanceled(waiter.CancellationToken);
    }

    private ulong? UpdateLocalWaitersUnderLock(ZLinkApplicationJobOrigin origin, int delta)
    {
        if (origin != ZLinkApplicationJobOrigin.Local)
            return null;
        _localWaiters += delta;
        if (_localWaiters == 0)
            _localBacklogLogged = false;
        if (
            !_localBacklogLogged
            && (ulong)_localWaiters > _capacity.EffectiveMaxQueuedApplicationJobs
        )
        {
            _localBacklogLogged = true;
            return (ulong)_localWaiters;
        }
        return null;
    }

    private void LogLocalBacklog(ulong localWaiters)
    {
        try
        {
            _logger?.LogWarning(
                "zlink.runtime.host.local_job_backlog_exceeded source_kind={source_kind} source_name={source_name} local_waiters={local_waiters} effective_maximum={effective_maximum}",
                "host",
                "zlink.runtime.host",
                localWaiters,
                _capacity.EffectiveMaxQueuedApplicationJobs
            );
        }
        catch (Exception error)
        {
            _loggerFailureReporter?.Invoke(error);
        }
    }

    private void RecordCompletedWaitUnderLock(Waiter waiter, (long Now, long Frequency) clock)
    {
        UpdateLocalWaitersUnderLock(waiter.Origin, -1);
        if (waiter.MeasurementEpoch != _measurementEpoch)
            return;
        _capacityWaitCount = checked(_capacityWaitCount + 1);
        var elapsed = ElapsedSince(waiter.StartedTimestamp, clock);
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
        _peakPermitsInUse = Math.Max(_peakPermitsInUse, PermitsInUseUnderLock());
    }

    internal ValueTask<IAsyncDisposable> RegisterReceiveFlowSocketAsync(IRouterSocket socket)
    {
        ArgumentNullException.ThrowIfNull(socket);
        return RegisterReceiveFlowSocketAsync(socket, socket.SetReceiveFlowState);
    }

    internal IAsyncDisposable RegisterReceiveFlowSocket(
        object identity,
        Action<ReceiveFlowState> apply
    ) => RegisterReceiveFlowSocketAsync(identity, apply).GetAwaiter().GetResult();

    internal async ValueTask<IAsyncDisposable> RegisterReceiveFlowSocketAsync(
        object identity,
        Action<ReceiveFlowState> apply
    )
    {
        var registration = RegisterReceiveFlowSocketUnapplied(identity, apply);
        return await _receiveFlowController
            .ApplyRegistrationAsync(identity, registration)
            .ConfigureAwait(false);
    }

    internal IAsyncDisposable RegisterReceiveFlowSocketUnapplied(
        object identity,
        Action<ReceiveFlowState> apply
    )
    {
        ArgumentNullException.ThrowIfNull(identity);
        ArgumentNullException.ThrowIfNull(apply);
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            return _receiveFlowController.Register(
                identity,
                apply,
                _pressureState,
                _pressureTransitionSequence
            );
        }
    }

    internal void ApplyReceiveFlowRegistration(object identity, IAsyncDisposable registration) =>
        _receiveFlowController.ApplyRegistration(identity, registration);

    private bool UpdatePressureStateUnderLock((long Now, long Frequency) clock)
    {
        var permits = PermitsInUseUnderLock();
        var next = _pressureState;
        if (
            _pressureState == ZLinkApplicationJobQueuePressureState.Running
            && permits >= _capacity.PausePermitCount
        )
            next = ZLinkApplicationJobQueuePressureState.Paused;
        else if (
            _pressureState == ZLinkApplicationJobQueuePressureState.Paused
            && permits <= _capacity.ResumePermitCount
        )
            next = ZLinkApplicationJobQueuePressureState.Running;
        if (next == _pressureState)
            return false;

        var now = clock.Now;
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
            AddCumulativePauseUnderLock(ElapsedSince(_cumulativePauseStartedTimestamp, clock));
            _pauseStartedTimestamp = 0;
            _cumulativePauseStartedTimestamp = 0;
        }
        _receiveFlowController.Transition(next, _pressureTransitionSequence);
        return true;
    }

    private (long Now, long Frequency) ReadClock() =>
        (_timeProvider.GetTimestamp(), _timeProvider.TimestampFrequency);

    private TimeSpan CurrentPauseDurationUnderLock((long Now, long Frequency) clock) =>
        _pressureState == ZLinkApplicationJobQueuePressureState.Paused
            ? ElapsedSince(_pauseStartedTimestamp, clock)
            : TimeSpan.Zero;

    private TimeSpan CumulativePauseDurationUnderLock((long Now, long Frequency) clock)
    {
        if (_pressureState != ZLinkApplicationJobQueuePressureState.Paused)
            return _cumulativePauseDuration;
        return AddSaturated(
            _cumulativePauseDuration,
            ElapsedSince(_cumulativePauseStartedTimestamp, clock)
        );
    }

    private static TimeSpan ElapsedSince(long startedTimestamp, (long Now, long Frequency) clock)
    {
        if (clock.Now <= startedTimestamp)
            return TimeSpan.Zero;
        return new TimeSpan(
            (long)(
                (clock.Now - startedTimestamp) * ((double)TimeSpan.TicksPerSecond / clock.Frequency)
            )
        );
    }

    private void AddCumulativePauseUnderLock(TimeSpan duration) =>
        _cumulativePauseDuration = AddSaturated(_cumulativePauseDuration, duration);

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
        var clock = ReadClock();
        (Waiter Waiter, CancellationTokenRegistration Registration)[] waiters;
        lock (_sync)
        {
            if (_disposed)
                return;
            _disposed = true;
            _receiveFlowController.BeginClose();
            waiters = new (Waiter, CancellationTokenRegistration)[_waiters.Count];
            var index = 0;
            foreach (var waiter in _waiters)
            {
                waiter.Node = null;
                waiter.State = WaiterState.Cancelled;
                _capacityWaiters = checked(_capacityWaiters - 1);
                RecordCompletedWaitUnderLock(waiter, clock);
                waiters[index++] = (waiter, waiter.CancellationRegistration);
            }
            _waiters.Clear();
        }
        foreach (var (waiter, registration) in waiters)
        {
            registration.Dispose();
            waiter.Completion.TrySetException(
                new ObjectDisposedException(nameof(ZLinkApplicationJobQueue))
            );
        }
    }

    private sealed class Waiter(
        CancellationToken cancellationToken,
        long startedTimestamp,
        ulong measurementEpoch,
        ZLinkPreparedWork<ZLinkApplicationJobQueueLease?>? publication,
        ZLinkApplicationJobOrigin origin,
        int maximum
    )
    {
        internal int Maximum { get; } = maximum;
        internal ZLinkApplicationJobOrigin Origin { get; } = origin;
        internal CancellationToken CancellationToken { get; } = cancellationToken;
        internal long StartedTimestamp { get; } = startedTimestamp;
        internal ulong MeasurementEpoch { get; } = measurementEpoch;
        internal ZLinkPreparedWork<ZLinkApplicationJobQueueLease?>? Publication { get; } =
            publication;
        internal TaskCompletionSource<ZLinkApplicationJobQueueReservation> Completion { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal LinkedListNode<Waiter>? Node { get; set; }
        internal CancellationTokenRegistration CancellationRegistration { get; set; }
        internal WaiterState State { get; set; }
    }

    private sealed record CancellationRegistrationState(
        ZLinkApplicationJobQueue Owner,
        Waiter Waiter
    );

    private enum WaiterState
    {
        Waiting = 0,
        Admitted = 1,
        Cancelled = 2,
    }

    private readonly record struct AcquireResult(
        Waiter? Waiter,
        ZLinkApplicationJobQueueReservation Reservation,
        bool PressureChanged,
        ulong? LocalBacklogWarning = null,
        bool PublicationEnqueued = false
    );

    private readonly record struct ReleaseResult(
        Waiter? AdmittedWaiter,
        ZLinkApplicationJobQueueReservation Reservation,
        bool PressureChanged,
        CancellationTokenRegistration CancellationRegistration = default,
        bool PublicationEnqueued = false
    );
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
    private readonly object _sync;
    private readonly Action<Exception>? _failureReporter;
    private readonly Dictionary<object, Entry> _entries = new(ReferenceEqualityComparer.Instance);
    private ulong _flowStateConfigFailures;

    internal ZLinkReceiveFlowController(object sync, Action<Exception>? failureReporter)
    {
        _sync = sync;
        _failureReporter = failureReporter;
    }

    internal ulong FlowStateConfigFailures
    {
        get
        {
            lock (_sync)
                return _flowStateConfigFailures;
        }
    }

    internal IAsyncDisposable Register(
        object identity,
        Action<ReceiveFlowState> apply,
        ZLinkApplicationJobQueuePressureState state,
        ulong sequence
    )
    {
        // The queue checks disposal and supplies its current pressure under this same lock.
        Entry entry;
        lock (_sync)
        {
            if (_entries.TryGetValue(identity, out entry!))
                entry.ReferenceCount = checked(entry.ReferenceCount + 1);
            else
            {
                entry = new Entry(identity, apply);
                entry.Pending.Enqueue(new FlowUpdate(sequence, state));
                _entries.Add(identity, entry);
            }
        }
        return new Registration(this, entry);
    }

    internal async ValueTask<IAsyncDisposable> ApplyRegistrationAsync(
        object identity,
        IAsyncDisposable registration
    )
    {
        try
        {
            ApplyRegistration(identity, registration);
            return registration;
        }
        catch
        {
            await registration.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    internal void ApplyRegistration(object identity, IAsyncDisposable registration)
    {
        var entry = ((Registration)registration).Entry;
        Apply(entry, rethrowUnexpected: true);
        lock (_sync)
            ObjectDisposedException.ThrowIf(
                entry.Removed
                    || !_entries.TryGetValue(identity, out var current)
                    || !ReferenceEquals(current, entry),
                this
            );
    }

    internal void Transition(ZLinkApplicationJobQueuePressureState state, ulong sequence)
    {
        lock (_sync)
            foreach (var entry in _entries.Values)
                entry.Pending.Enqueue(new FlowUpdate(sequence, state));
    }

    internal void ApplyPending()
    {
        Entry[] entries;
        lock (_sync)
            entries = _entries.Count == 0 ? [] : _entries.Values.ToArray();
        foreach (var entry in entries)
            Apply(entry, rethrowUnexpected: false);
    }

    internal void BeginClose()
    {
        lock (_sync)
        {
            foreach (var entry in _entries.Values)
            {
                entry.Removed = true;
                entry.Pending.Clear();
            }
            _entries.Clear();
        }
    }

    internal void ResetMetrics()
    {
        lock (_sync)
            _flowStateConfigFailures = 0;
    }

    private void Apply(Entry entry, bool rethrowUnexpected)
    {
        while (true)
        {
            ApplyPreparation preparation;
            lock (_sync)
                preparation = PrepareApply(entry);
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

            (bool ExpectedCloseRace, TaskCompletionSource Completion) completed;
            lock (_sync)
                completed = CompleteApply(entry, preparation.Update, failure);
            completed.Completion.TrySetResult();

            if (failure is null || completed.ExpectedCloseRace)
                continue;
            try
            {
                _failureReporter?.Invoke(failure);
            }
            catch { }
            if (rethrowUnexpected)
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
        }
    }

    internal static ValueTask DisposeRegistrationAsync(IAsyncDisposable? registration) =>
        registration is null ? ValueTask.CompletedTask : registration.DisposeAsync();

    private async ValueTask UnregisterAsync(Entry entry)
    {
        Task? applying;
        lock (_sync)
        {
            if (!entry.Removed)
            {
                entry.ReferenceCount--;
                if (entry.ReferenceCount == 0)
                {
                    entry.Removed = true;
                    entry.Pending.Clear();
                    _entries.Remove(entry.Identity);
                }
            }
            applying = entry.ApplyCompleted?.Task;
        }
        // Finish a started binding call before the socket owner closes its handle.
        if (applying is not null && !ReferenceEquals(CurrentApplyingEntry.Value, entry))
            await applying.ConfigureAwait(false);
    }

    private ApplyPreparation PrepareApply(Entry entry)
    {
        if (entry.Removed || entry.Pending.Count == 0 || entry.ApplyCompleted is not null)
            return default;
        entry.ApplyCompleted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        return new ApplyPreparation(true, entry.Pending.Peek());
    }

    private (bool ExpectedCloseRace, TaskCompletionSource Completion) CompleteApply(
        Entry entry,
        FlowUpdate update,
        Exception? failure
    )
    {
        if (entry.Pending.Count > 0 && entry.Pending.Peek() == update)
            entry.Pending.Dequeue();
        var expectedCloseRace =
            entry.Removed
            && failure
                is ZlinkConfigException { Result: ZlinkConfigException.ErrorCode.InvalidState };
        if (failure is not null && !expectedCloseRace)
            _flowStateConfigFailures = checked(_flowStateConfigFailures + 1);
        var completion = entry.ApplyCompleted!;
        entry.ApplyCompleted = null;
        return (expectedCloseRace, completion);
    }

    private static ReceiveFlowState ToBindingState(ZLinkApplicationJobQueuePressureState state) =>
        state switch
        {
            ZLinkApplicationJobQueuePressureState.Running => ReceiveFlowState.Running,
            ZLinkApplicationJobQueuePressureState.Paused => ReceiveFlowState.Paused,
            _ => throw new ArgumentOutOfRangeException(nameof(state)),
        };

    private sealed class Entry(object identity, Action<ReceiveFlowState> apply)
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
        ZLinkApplicationJobQueuePressureState State
    );

    private readonly record struct ApplyPreparation(bool ShouldApply, FlowUpdate Update);

    private sealed class Registration : IAsyncDisposable
    {
        private readonly Entry _entry;
        private ZLinkReceiveFlowController? _owner;

        internal Registration(ZLinkReceiveFlowController owner, Entry entry)
        {
            _owner = owner;
            _entry = entry;
        }

        internal Entry Entry => _entry;

        public ValueTask DisposeAsync() =>
            Interlocked.Exchange(ref _owner, null)?.UnregisterAsync(_entry)
            ?? ValueTask.CompletedTask;
    }
}

internal sealed class ZLinkApplicationJobQueueLease : IDisposable
{
    private readonly ZLinkApplicationJobQueue _owner;
    private int _state;

    internal ZLinkApplicationJobOrigin Origin { get; }

    internal ZLinkApplicationJobQueueLease(
        ZLinkApplicationJobQueue owner,
        ZLinkApplicationJobOrigin origin
    )
    {
        _owner = owner;
        Origin = origin;
    }

    internal void MarkQueued()
    {
        // The state is monotonic: a queued/released lease cannot become reserved again.
        if (Volatile.Read(ref _state) == (int)LeaseState.Reserved)
            _owner.MarkQueued(this);
    }

    internal ZLinkApplicationJobQueue Owner => _owner;

    internal bool IsReleased => Volatile.Read(ref _state) == (int)LeaseState.Released;

    internal ValueTask ReleaseForHandlerStartAsync() => _owner.ReleaseForHandlerStartAsync(this);

    internal bool TryMarkQueued() =>
        Interlocked.CompareExchange(ref _state, (int)LeaseState.Queued, (int)LeaseState.Reserved)
        == (int)LeaseState.Reserved;

    internal LeaseState TryRelease() =>
        (LeaseState)Interlocked.Exchange(ref _state, (int)LeaseState.Released);

    public void Dispose()
    {
        if (!IsReleased)
            _owner.Release(this);
    }

    internal enum LeaseState
    {
        Reserved = 0,
        Queued = 1,
        Released = 2,
    }
}

/// <summary>
/// One receive rotation's permits, reserved on the queue before any record is
/// claimed (spec 08 I1). A lease exists only for a claimed record: the
/// reserved count stays in <see cref="ZLinkApplicationJobQueue"/>'s reserved
/// supply counter, and <see cref="Dispose"/> is the one place that returns the
/// permits no record took. Without a queue, claims carry no lease.
/// </summary>
internal struct ZLinkApplicationJobQueueReservation : IDisposable
{
    private readonly ZLinkApplicationJobQueue? _owner;
    private readonly ZLinkApplicationJobOrigin _origin;
    private ZLinkApplicationJobQueueLease? _held;
    private int _unleased;

    internal ZLinkApplicationJobQueueReservation(
        ZLinkApplicationJobQueue? owner,
        ZLinkApplicationJobOrigin origin,
        ZLinkApplicationJobQueueLease? held,
        int unleased = 0
    )
    {
        _owner = owner;
        _origin = origin;
        _held = held;
        _unleased = unleased;
    }

    internal readonly int Count => (_held is null ? 0 : 1) + _unleased;

    /// <summary>Reserves the queue's available permits up to <paramref name="maximum"/> in all.</summary>
    internal void ReserveAvailable(int maximum) =>
        _unleased += _owner!.TryReserveBatch(Math.Max(0, maximum - Count));

    /// <summary>Takes one reserved permit for a claimed record.</summary>
    internal ZLinkApplicationJobQueueLease? Claim()
    {
        if (Count == 0)
            throw new InvalidOperationException("The reservation has no unclaimed permit.");
        if (_held is { } held)
        {
            _held = null;
            return held;
        }
        _unleased--;
        return _owner is null ? null : new ZLinkApplicationJobQueueLease(_owner, _origin);
    }

    public void Dispose()
    {
        var held = _held;
        var unleased = _unleased;
        _held = null;
        _unleased = 0;
        try
        {
            held?.Dispose();
        }
        finally
        {
            _owner?.ReleaseReserved(unleased);
        }
    }
}

internal sealed class ZLinkApplicationJobQueueRecordOwner : IDisposable
{
    private IDisposable? _payloadOwner;
    private ZLinkApplicationJobQueueLease? _admission;

    internal ZLinkApplicationJobQueueRecordOwner(
        IDisposable? payloadOwner,
        ZLinkApplicationJobQueueLease admission
    )
    {
        _payloadOwner = payloadOwner;
        _admission = admission;
    }

    internal ZLinkApplicationJobQueueLease? Admission => Volatile.Read(ref _admission);

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

    // Handler entry releases the queue permit, but the invocation still owns its turn.
    internal static bool IsActive => Current.Value is not null;

    internal static ZLinkApplicationJobOrigin? CurrentOrigin => Current.Value?.Origin;
    internal static Scope? CurrentScope => Current.Value;

    internal static IDisposable Enter(ZLinkApplicationJobQueueLease lease)
    {
        ArgumentNullException.ThrowIfNull(lease);
        var scope = new Scope(Current.Value, lease);
        Current.Value = scope;
        return scope;
    }

    internal static ValueTask ReleaseForHandlerStartAsync()
    {
        var scope = Current.Value;
        return scope is null
            ? ValueTask.CompletedTask
            : Interlocked.Exchange(ref scope.Lease, null)?.ReleaseForHandlerStartAsync()
                ?? ValueTask.CompletedTask;
    }

    // A mesh mailbox claim may cross exactly one owner-queue admission
    // boundary without another capacity decision. Consuming this marker does
    // not release the host-wide permit; the invocation boundary still returns
    // that permit immediately before the handler's first instruction.
    internal static bool HasTransferableOwnerReservation()
    {
        var scope = Current.Value;
        return scope is not null
            && Volatile.Read(ref scope.Lease) is { IsReleased: false }
            && Volatile.Read(ref scope.OwnerReservationTransferred) == 0;
    }

    internal static bool TryTransferOwnerReservation()
    {
        var scope = Current.Value;
        return scope is not null
            && Volatile.Read(ref scope.Lease) is { IsReleased: false }
            && Interlocked.CompareExchange(ref scope.OwnerReservationTransferred, 1, 0) == 0;
    }

    internal static async ValueTask EnsureQueuedPermitAsync(CancellationToken cancellationToken)
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

            var acquired = await scope
                .Owner.AcquireAsync(cancellationToken, scope.Origin)
                .ConfigureAwait(false);
            if (Interlocked.CompareExchange(ref scope.Lease, acquired, current) == current)
            {
                acquired.MarkQueued();
                return;
            }

            acquired.Dispose();
        }
    }

    internal sealed class Scope(Scope? previous, ZLinkApplicationJobQueueLease lease) : IDisposable
    {
        private readonly Scope? _previous = previous;
        internal readonly ZLinkApplicationJobQueue Owner = lease.Owner;
        internal readonly ZLinkApplicationJobOrigin Origin = lease.Origin;
        internal ZLinkApplicationJobQueueLease? Lease = lease;
        internal int OwnerReservationTransferred;
        private int _disposed;

        internal void ReleaseDurablePermit() => Interlocked.Exchange(ref Lease, null)?.Dispose();

        internal async ValueTask ReacquireAsync(CancellationToken cancellationToken)
        {
            var acquired = await Owner
                .AcquireAsync(cancellationToken, Origin)
                .ConfigureAwait(false);
            if (Interlocked.CompareExchange(ref Lease, acquired, null) is not null)
            {
                acquired.Dispose();
                throw new InvalidOperationException(
                    "The durable Actor record already owns a permit."
                );
            }
            if (Volatile.Read(ref _disposed) != 0 || cancellationToken.IsCancellationRequested)
            {
                ReleaseDurablePermit();
                throw new OperationCanceledException(cancellationToken);
            }
            acquired.MarkQueued();
        }

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
