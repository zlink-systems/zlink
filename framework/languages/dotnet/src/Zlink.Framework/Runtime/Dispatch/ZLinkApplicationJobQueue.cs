namespace Zlink.Framework.Runtime.Dispatch;

internal readonly record struct ZLinkApplicationJobQueueCapacity(
    ZLinkApplicationJobQueueProfile ConfiguredProfile,
    ulong? ConfiguredManualMax,
    ulong EffectiveProcessorCount,
    ulong EffectiveMaxQueuedApplicationJobs);

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
        ulong effectiveProcessorCount)
    {
        if (!Enum.IsDefined(profile))
            throw new ZLinkConfigurationException(
                $"Unknown ApplicationJobQueueProfile value '{(int)profile}'.");
        if (configuredManualMax is 0 or > MaximumQueueLimit)
            throw new ZLinkConfigurationException(
                $"MaxQueuedApplicationJobs must be between 1 and {MaximumQueueLimit}.");

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
            configuredManualMax ?? automatic);
    }
}

/// <summary>
/// Owns the host-wide FIFO permit lifecycle. A permit accounts only a reserved
/// supply record or a queued application job; running user code is outside it.
/// </summary>
internal sealed class ZLinkApplicationJobQueue : IDisposable
{
    private readonly object _gate;
    private readonly TimeProvider _timeProvider;
    private readonly LinkedList<Waiter> _waiters = new();
    private readonly ZLinkApplicationJobQueueCapacity _capacity;
    private ulong _reservedSupplyPermits;
    private ulong _queuedApplicationJobs;
    private ulong _peakPermitsInUse;
    private ulong _capacityWaiters;
    private ulong _capacityWaitCount;
    private TimeSpan _capacityWaitDuration;
    private bool _disposed;

    internal ZLinkApplicationJobQueue(
        ZLinkApplicationJobQueueCapacity capacity,
        object? capacityGate = null,
        TimeProvider? timeProvider = null)
    {
        if (capacity.EffectiveMaxQueuedApplicationJobs is 0 or > int.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        _capacity = capacity;
        _gate = capacityGate ?? new object();
        _timeProvider = timeProvider ?? TimeProvider.System;
    }

    internal object SyncRoot => _gate;

    internal ValueTask<ZLinkApplicationJobQueueLease> AcquireAsync(
        CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
            return ValueTask.FromCanceled<ZLinkApplicationJobQueueLease>(
                cancellationToken);

        Waiter? waiter = null;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_waiters.Count == 0
                && PermitsInUseUnderLock()
                < _capacity.EffectiveMaxQueuedApplicationJobs)
            {
                _reservedSupplyPermits = checked(_reservedSupplyPermits + 1);
                ObservePeakUnderLock();
                return ValueTask.FromResult(
                    new ZLinkApplicationJobQueueLease(this));
            }

            waiter = new Waiter(
                cancellationToken,
                _timeProvider.GetTimestamp());
            waiter.Node = _waiters.AddLast(waiter);
            _capacityWaiters = checked(_capacityWaiters + 1);
        }

        if (cancellationToken.CanBeCanceled)
        {
            var registration = cancellationToken.UnsafeRegister(
                static state =>
                {
                    var registrationState = (CancellationRegistrationState)state!;
                    registrationState.Owner.CancelWaiter(
                        registrationState.Waiter);
                },
                new CancellationRegistrationState(this, waiter));
            lock (_gate)
            {
                waiter.CancellationRegistration = registration;
                if (waiter.State != WaiterState.Waiting)
                    registration.Dispose();
            }
        }

        return new ValueTask<ZLinkApplicationJobQueueLease>(waiter.Completion.Task);
    }

    internal ZLinkApplicationJobQueueStatus GetStatus()
    {
        lock (_gate)
            return GetStatusUnderLock();
    }

    internal ZLinkApplicationJobQueueStatus GetStatusUnderLock()
    {
        if (!Monitor.IsEntered(_gate))
            throw new SynchronizationLockException(
                "The host-capacity gate must be held while reading queue status.");
        return new ZLinkApplicationJobQueueStatus(
            _capacity.ConfiguredProfile,
            _capacity.ConfiguredManualMax,
            _capacity.EffectiveProcessorCount,
            _capacity.EffectiveMaxQueuedApplicationJobs,
            _reservedSupplyPermits,
            _queuedApplicationJobs,
            PermitsInUseUnderLock(),
            _peakPermitsInUse,
            _capacityWaiters,
            _capacityWaitCount,
            _capacityWaitDuration);
    }

    internal void ResetMetrics()
    {
        lock (_gate)
            ResetMetricsUnderLock();
    }

    internal void ResetMetricsUnderLock()
    {
        if (!Monitor.IsEntered(_gate))
            throw new SynchronizationLockException(
                "The host-capacity gate must be held while resetting queue metrics.");
        _peakPermitsInUse = PermitsInUseUnderLock();
        _capacityWaitCount = 0;
        _capacityWaitDuration = TimeSpan.Zero;
    }

    internal void MarkQueued(ZLinkApplicationJobQueueLease lease)
    {
        lock (_gate)
        {
            if (!lease.TryMarkQueued())
                return;
            _reservedSupplyPermits = checked(_reservedSupplyPermits - 1);
            _queuedApplicationJobs = checked(_queuedApplicationJobs + 1);
        }
    }

    internal void Release(ZLinkApplicationJobQueueLease lease)
    {
        Waiter? admittedWaiter = null;
        ZLinkApplicationJobQueueLease? admittedLease = null;
        lock (_gate)
        {
            var previous = lease.TryRelease();
            if (previous == ZLinkApplicationJobQueueLease.LeaseState.Released)
                return;
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
                RecordCompletedWaitUnderLock(candidate.StartedTimestamp);
                _reservedSupplyPermits = checked(_reservedSupplyPermits + 1);
                ObservePeakUnderLock();
                admittedWaiter = candidate;
                admittedLease = new ZLinkApplicationJobQueueLease(this);
                break;
            }
        }

        if (admittedWaiter is not null)
        {
            admittedWaiter.CancellationRegistration.Dispose();
            admittedWaiter.Completion.TrySetResult(admittedLease!);
        }
    }

    private void CancelWaiter(Waiter waiter)
    {
        lock (_gate)
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
            RecordCompletedWaitUnderLock(waiter.StartedTimestamp);
        }
        waiter.Completion.TrySetCanceled(waiter.CancellationToken);
    }

    private void RecordCompletedWaitUnderLock(long startedTimestamp)
    {
        _capacityWaitCount = checked(_capacityWaitCount + 1);
        var elapsed = _timeProvider.GetElapsedTime(
            startedTimestamp,
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

    public void Dispose()
    {
        Waiter[] waiters;
        lock (_gate)
        {
            if (_disposed)
                return;
            _disposed = true;
            waiters = _waiters.ToArray();
            _waiters.Clear();
            foreach (var waiter in waiters)
            {
                waiter.Node = null;
                if (waiter.State != WaiterState.Waiting)
                    continue;
                waiter.State = WaiterState.Cancelled;
                _capacityWaiters = checked(_capacityWaiters - 1);
                RecordCompletedWaitUnderLock(waiter.StartedTimestamp);
            }
        }
        foreach (var waiter in waiters)
        {
            waiter.CancellationRegistration.Dispose();
            waiter.Completion.TrySetException(
                new ObjectDisposedException(nameof(ZLinkApplicationJobQueue)));
        }
    }

    private sealed class Waiter(
        CancellationToken cancellationToken,
        long startedTimestamp)
    {
        internal CancellationToken CancellationToken { get; } = cancellationToken;
        internal long StartedTimestamp { get; } = startedTimestamp;
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

internal sealed class ZLinkApplicationJobQueueCreditOwner : IDisposable
{
    private IDisposable? _coreCreditOwner;
    private ZLinkApplicationJobQueueLease? _admission;

    internal ZLinkApplicationJobQueueCreditOwner(
        IDisposable? coreCreditOwner,
        ZLinkApplicationJobQueueLease admission)
    {
        _coreCreditOwner = coreCreditOwner;
        _admission = admission;
    }

    internal ZLinkApplicationJobQueueLease? Admission =>
        Volatile.Read(ref _admission);

    public void Dispose()
    {
        try
        {
            Interlocked.Exchange(ref _coreCreditOwner, null)?.Dispose();
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
