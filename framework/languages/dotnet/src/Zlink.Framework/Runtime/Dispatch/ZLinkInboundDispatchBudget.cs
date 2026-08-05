using System.Runtime.InteropServices;

namespace Zlink.Framework.Runtime.Dispatch;

internal sealed class ZLinkInboundDispatchBudget
{
    internal const ulong DefaultMaximumMessageBytes = 16UL * 1024 * 1024;
    internal const int DefaultMaximumConcurrentReceives = 16;

    private readonly object _gate = new();
    private readonly SemaphoreSlim _receiveCapacity = new(1, 1);
    private readonly SemaphoreSlim _receiveWake = new(0);
    private readonly ulong _applicationHwmBytes;
    private readonly ulong _maximumMessageBytes;
    private readonly int _maximumConcurrentReceives;
    private readonly ZLinkInboundDispatchMetricShard[] _activePayloadShards =
        CreateMetricShards();
    private ulong _pendingPayloadBytes;
    private int _activeReceiveReservations;
    private int _overageReservations;
    private int _receivePaused;
    private List<Action>? _capacityAvailableHandlers;

    internal ZLinkInboundDispatchBudget(
        ulong applicationHwmBytes,
        ulong maximumMessageBytes = DefaultMaximumMessageBytes,
        int maximumConcurrentReceives = DefaultMaximumConcurrentReceives)
    {
        if (maximumMessageBytes == 0)
            throw new ArgumentOutOfRangeException(nameof(maximumMessageBytes));
        if (maximumConcurrentReceives <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumConcurrentReceives));
        _applicationHwmBytes = applicationHwmBytes;
        _maximumMessageBytes = maximumMessageBytes;
        _maximumConcurrentReceives = maximumConcurrentReceives;
    }

    internal bool CanStartApplicationReceive
        => CanStartApplicationReceiveNow();

    internal ulong PendingPayloadBytes
        => Volatile.Read(ref _pendingPayloadBytes);

    internal ulong MaximumMessageBytes => _maximumMessageBytes;

    internal int MaximumConcurrentReceives => _maximumConcurrentReceives;

    internal int OverageReservationCount =>
        Volatile.Read(ref _overageReservations);

    // The binding cannot expose a complete message length before Recv. A
    // bounded receive reservation makes the documented overshoot explicit:
    // HWM + (concurrent receives * maximum message size).
    internal ulong MaximumPendingPayloadBytes =>
        _applicationHwmBytes == 0
            ? ulong.MaxValue
            : checked(_applicationHwmBytes
                      + checked((ulong)_maximumConcurrentReceives
                                * _maximumMessageBytes));

    internal async ValueTask<ZLinkInboundReceivePermit> AcquireReceiveAsync(
        CancellationToken cancellationToken)
    {
        while (true)
        {
            if (TryAcquireReceive(out var permit))
                return permit;

            await _receiveWake.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
    }

    // A multiplexed native receive loop must not wait for application
    // capacity. When all fixed permits for classifying a raw message are in
    // use, the caller returns to the poller so request completions and
    // infrastructure control can continue while application payload remains
    // above its HWM.
    internal bool TryAcquireReceive(out ZLinkInboundReceivePermit permit)
    {
        if (_applicationHwmBytes == 0)
        {
            permit = new ZLinkInboundReceivePermit(
                this,
                ownsReservation: false);
            return true;
        }

        if (!TryReserveReceive(out var overageReservation))
        {
            permit = null!;
            return false;
        }

        permit = new ZLinkInboundReceivePermit(
            this,
            ownsReservation: true,
            overageReservation: overageReservation);
        return true;
    }

    internal async ValueTask<bool> WaitForReceiveCapacityAsync(
        CancellationToken cancellationToken)
    {
        while (true)
        {
            if (CanStartApplicationReceiveNow()
                && Volatile.Read(ref _receivePaused) == 0)
                return false;

            await _receiveCapacity.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            // The permit represents one receive attempt released by a
            // completed dispatch. A concurrent receive may cross the HWM
            // again before this waiter resumes; the attempt still owns the
            // permit and CompleteReceiveAttempt reconciles the next state.
            return true;
        }
    }

    internal void CompleteReceiveAttempt(bool ownsResumePermit)
    {
        if (!ownsResumePermit)
            return;
        lock (_gate)
        {
            if (CanStartApplicationReceiveUnderLock())
            {
                Volatile.Write(ref _receivePaused, 0);
                ReleaseReceiveCapacityUnderLock();
            }
            else
            {
                Volatile.Write(ref _receivePaused, 1);
                _receiveCapacity.Wait(0);
            }
        }
    }

    internal void CompleteReceiveAttempt(ZLinkInboundReceivePermit? permit)
    {
        if (permit is null || !permit.TryComplete())
            return;

        if (permit.OwnsReservation)
        {
            ReleaseReceiveReservation();
        }
        if (permit.TryReleaseOverageReservation())
            ReleaseOverageReservation();
        SignalReceiveWaiters();
    }

    internal void Received(ulong payloadBytes)
    {
        var pending = AddPendingPayload(payloadBytes);
        MarkReceivePausedIfNeeded(pending);
    }

    internal bool Received(
        ZLinkInboundReceivePermit permit,
        ulong payloadBytes)
    {
        permit.ValidateOwner(this);
        if (!permit.TryMarkApplicationReceived())
            throw new InvalidOperationException(
                "An inbound receive permit was used more than once.");

        var pending = AddPendingPayload(payloadBytes);
        var overageReservation = _applicationHwmBytes != 0
            && pending >= _applicationHwmBytes
            && (permit.TryTakeOverageReservation()
                || TryReserveOverageSlot());
        MarkReceivePausedIfNeeded(pending);
        return overageReservation;
    }

    internal bool TryTrack(
        ulong payloadBytes,
        out ZLinkInboundDispatchLease? lease)
    {
        while (true)
        {
            var current = Volatile.Read(ref _pendingPayloadBytes);
            // A complete message that began below HWM may finish above it;
            // the contract stops the next application receive, so admission
            // checks the immutable pending total before this message.
            if (_applicationHwmBytes != 0
                && current >= _applicationHwmBytes)
            {
                lease = null;
                return false;
            }

            var next = checked(current + payloadBytes);
            var overageReservation = _applicationHwmBytes != 0
                && next >= _applicationHwmBytes;
            if (overageReservation && !TryReserveOverageSlot())
            {
                lease = null;
                return false;
            }
            if (Interlocked.CompareExchange(
                    ref _pendingPayloadBytes,
                    next,
                    current) != current)
            {
                if (overageReservation)
                    ReleaseOverageReservation();
                continue;
            }
            MarkReceivePausedIfNeeded(next);
            lease = new ZLinkInboundDispatchLease(
                this,
                payloadBytes,
                overageReservation);
            return true;
        }
    }

    internal void HandlerStarted(ulong payloadBytes)
    {
        AddActivePayload(payloadBytes);
    }

    internal void Completed(
        ulong payloadBytes,
        bool handlerStarted,
        bool overageReservation = false)
    {
        Action[]? handlers = null;
        if (handlerStarted)
        {
            SubtractActivePayload(payloadBytes);
        }
        var pending = SubtractPendingPayload(payloadBytes);
        if (overageReservation)
            ReleaseOverageReservation();
        SignalReceiveWaiters();
        if (_applicationHwmBytes != 0
            && pending < _applicationHwmBytes)
        {
            if (Volatile.Read(ref _receivePaused) != 0)
            {
                lock (_gate)
                {
                    if (Volatile.Read(ref _receivePaused) != 0
                        && CanStartApplicationReceiveNow())
                    {
                        Volatile.Write(ref _receivePaused, 0);
                        ReleaseReceiveCapacityUnderLock();
                        handlers = _capacityAvailableHandlers?.ToArray();
                    }
                }
            }
        }

        if (handlers is null) return;
        foreach (var handler in handlers)
        {
            try
            {
                handler();
            }
            catch
            {
                // Capacity notification must not change payload accounting or
                // turn a completed application dispatch into a runtime failure.
            }
        }
    }

    internal ZLinkInboundDispatchLease? Track(ulong payloadBytes)
    {
        return TryTrack(payloadBytes, out var lease) ? lease : null;
    }

    internal ZLinkInboundDispatchLease TrackReceived(
        ZLinkInboundReceivePermit permit,
        ulong payloadBytes)
    {
        var overageReservation = Received(permit, payloadBytes);
        return new ZLinkInboundDispatchLease(
            this,
            payloadBytes,
            overageReservation);
    }

    internal ZLinkInboundDispatchLease? Track(IReadOnlyList<Message> parts)
    {
        ulong payloadBytes = 0;
        foreach (var part in parts)
            payloadBytes = checked(payloadBytes + (ulong)part.Size);
        return Track(payloadBytes);
    }

    internal IDisposable RegisterCapacityAvailable(Action handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        lock (_gate)
        {
            (_capacityAvailableHandlers ??= []).Add(handler);
        }
        return new CapacityRegistration(this, handler);
    }

    private void UnregisterCapacityAvailable(Action handler)
    {
        lock (_gate)
            _capacityAvailableHandlers?.Remove(handler);
    }

    private sealed class CapacityRegistration(
        ZLinkInboundDispatchBudget owner,
        Action handler) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                owner.UnregisterCapacityAvailable(handler);
        }
    }

    internal ZLinkInboundDispatchBudgetSnapshot Snapshot()
    {
        var pending = Volatile.Read(ref _pendingPayloadBytes);
        var active = Math.Min(ReadActivePayload(), pending);
        return new ZLinkInboundDispatchBudgetSnapshot(
            _applicationHwmBytes,
            pending,
            pending - active,
            active,
            Volatile.Read(ref _receivePaused) != 0);
    }

    private bool CanStartApplicationReceiveNow() =>
        _applicationHwmBytes == 0
        || Volatile.Read(ref _pendingPayloadBytes) < _applicationHwmBytes;

    private bool CanStartApplicationReceiveUnderLock() =>
        _applicationHwmBytes == 0
        || Volatile.Read(ref _pendingPayloadBytes) < _applicationHwmBytes;

    private bool TryReserveReceive(out bool overageReservation)
    {
        overageReservation = false;
        while (true)
        {
            var active = Volatile.Read(ref _activeReceiveReservations);
            if (active >= _maximumConcurrentReceives)
                return false;
            if (Interlocked.CompareExchange(
                    ref _activeReceiveReservations,
                    active + 1,
                    active) != active)
                continue;

            if (_applicationHwmBytes != 0
                && Volatile.Read(ref _pendingPayloadBytes)
                   >= _applicationHwmBytes)
            {
                if (!TryReserveOverageSlot())
                {
                    ReleaseReceiveReservation();
                    return false;
                }
                overageReservation = true;
            }
            return true;
        }
    }

    private ulong AddPendingPayload(ulong payloadBytes) =>
        AddPayload(ref _pendingPayloadBytes, payloadBytes);

    private ulong SubtractPendingPayload(ulong payloadBytes) =>
        SubtractPayload(ref _pendingPayloadBytes, payloadBytes);

    private bool TryReserveOverageSlot()
    {
        while (true)
        {
            var current = Volatile.Read(ref _overageReservations);
            if (current >= _maximumConcurrentReceives)
                return false;
            if (Interlocked.CompareExchange(
                    ref _overageReservations,
                    current + 1,
                    current) == current)
                return true;
        }
    }

    private void ReleaseReceiveReservation()
    {
        if (Interlocked.Decrement(ref _activeReceiveReservations) < 0)
            throw new InvalidOperationException(
                "Inbound receive reservation accounting underflowed.");
    }

    private void ReleaseOverageReservation()
    {
        if (Interlocked.Decrement(ref _overageReservations) < 0)
            throw new InvalidOperationException(
                "Inbound HWM overage accounting underflowed.");
    }

    private void AddActivePayload(ulong payloadBytes) =>
        ActivePayloadShard.Add(payloadBytes);

    private void SubtractActivePayload(ulong payloadBytes) =>
        ActivePayloadShard.Subtract(payloadBytes);

    private ZLinkInboundDispatchMetricShard ActivePayloadShard =>
        _activePayloadShards[
            Environment.CurrentManagedThreadId
            & (_activePayloadShards.Length - 1)];

    private ulong ReadActivePayload()
    {
        long total = 0;
        foreach (var shard in _activePayloadShards)
            total += shard.Read();
        return total > 0 ? (ulong)total : 0;
    }

    private static ZLinkInboundDispatchMetricShard[] CreateMetricShards()
    {
        var shards = new ZLinkInboundDispatchMetricShard[8];
        for (var index = 0; index < shards.Length; index++)
            shards[index] = new ZLinkInboundDispatchMetricShard();
        return shards;
    }

    private void MarkReceivePausedIfNeeded(ulong pendingPayloadBytes)
    {
        if (_applicationHwmBytes == 0
            || pendingPayloadBytes < _applicationHwmBytes)
            return;

        if (Volatile.Read(ref _receivePaused) != 0)
            return;

        lock (_gate)
        {
            if (Volatile.Read(ref _pendingPayloadBytes) >= _applicationHwmBytes
                && Volatile.Read(ref _receivePaused) == 0)
            {
                Volatile.Write(ref _receivePaused, 1);
                _receiveCapacity.Wait(0);
            }
        }
    }

    private static ulong AddPayload(ref ulong target, ulong payloadBytes)
    {
        while (true)
        {
            var current = Volatile.Read(ref target);
            var next = unchecked(current + payloadBytes);
            if (Interlocked.CompareExchange(ref target, next, current)
                == current)
                return next;
        }
    }

    private static ulong SubtractPayload(ref ulong target, ulong payloadBytes)
    {
        while (true)
        {
            var current = Volatile.Read(ref target);
            var next = unchecked(current - payloadBytes);
            if (Interlocked.CompareExchange(ref target, next, current)
                == current)
                return next;
        }
    }

    private void ReleaseReceiveCapacityUnderLock()
    {
        if (_receiveCapacity.CurrentCount == 0)
            _receiveCapacity.Release();
    }

    private void SignalReceiveWaiters()
    {
        if (_receiveWake.CurrentCount != 0)
            return;
        try
        {
            _receiveWake.Release();
        }
        catch (SemaphoreFullException)
        {
            // A waiter can consume the wakeup between the count check and
            // Release. The state is checked again after every wakeup.
        }
    }

}

[StructLayout(LayoutKind.Explicit, Size = 128)]
internal sealed class ZLinkInboundDispatchMetricShard
{
    [FieldOffset(0)]
    private long _value;

    internal void Add(ulong payloadBytes) =>
        Interlocked.Add(ref _value, checked((long)payloadBytes));

    internal void Subtract(ulong payloadBytes) =>
        Interlocked.Add(ref _value, -checked((long)payloadBytes));

    // A handler can start and complete on different threads, so one shard
    // can be temporarily negative while the aggregate remains valid.
    internal long Read() => Interlocked.Read(ref _value);
}

internal sealed class ZLinkInboundReceivePermit
{
    private readonly ZLinkInboundDispatchBudget _owner;
    private int _completed;
    private int _applicationReceived;
    private int _overageReservation;

    internal ZLinkInboundReceivePermit(
        ZLinkInboundDispatchBudget owner,
        bool ownsReservation,
        bool overageReservation = false)
    {
        _owner = owner;
        OwnsReservation = ownsReservation;
        _overageReservation = overageReservation ? 1 : 0;
    }

    internal bool OwnsReservation { get; }

    internal void ValidateOwner(ZLinkInboundDispatchBudget owner)
    {
        if (!ReferenceEquals(_owner, owner)
            || Volatile.Read(ref _completed) != 0)
            throw new InvalidOperationException(
                "The inbound receive permit is no longer available.");
    }

    internal bool TryMarkApplicationReceived() =>
        Interlocked.Exchange(ref _applicationReceived, 1) == 0;

    internal bool TryTakeOverageReservation() =>
        Interlocked.Exchange(ref _overageReservation, 0) != 0;

    internal bool TryReleaseOverageReservation() =>
        Interlocked.Exchange(ref _overageReservation, 0) != 0;

    internal bool TryComplete() =>
        Interlocked.Exchange(ref _completed, 1) == 0;
}

internal readonly record struct ZLinkInboundDispatchBudgetSnapshot(
    ulong ApplicationHwmBytes,
    ulong PendingPayloadBytes,
    ulong QueuedPayloadBytes,
    ulong ActivePayloadBytes,
    bool ApplicationReceivePaused);
