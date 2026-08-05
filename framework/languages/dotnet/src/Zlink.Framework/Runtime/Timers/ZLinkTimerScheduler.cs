namespace Zlink.Framework.Runtime.Timers;

// A runtime generation owns one scheduler for all logical Spot timers.  The
// scheduler owns the single deadline wait; a timer only owns state for its
// current callback while that callback is being dispatched.
internal sealed class ZLinkTimerScheduler : IAsyncDisposable
{
    private readonly object _gate = new();
    private readonly PriorityQueue<ScheduledTimer, ZLinkTimerScheduleKey> _queue = new();
    private readonly HashSet<ZLinkTimer> _timers = [];
    private readonly SemaphoreSlim _wake = new(0, 1);
    private readonly CancellationTokenSource _stopSource = new();
    private readonly Task _pump;
    private readonly object _disposeGate = new();
    private Task? _disposeTask;
    private long _nextSequence;
    private bool _closed;

    public ZLinkTimerScheduler()
    {
        _pump = RunAsync(_stopSource.Token);
    }

    internal int TimerCount
    {
        get
        {
            lock (_gate)
                return _timers.Count;
        }
    }

    internal int ScheduledEntryCount
    {
        get
        {
            lock (_gate)
                return _queue.Count;
        }
    }

    internal void Register(ZLinkTimer timer)
    {
        ArgumentNullException.ThrowIfNull(timer);
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            _timers.Add(timer);
        }
    }

    internal void Unregister(ZLinkTimer timer)
    {
        lock (_gate)
            _timers.Remove(timer);
        SignalWake();
    }

    internal void Schedule(
        ZLinkTimer timer,
        DateTimeOffset dueAt,
        long version)
    {
        lock (_gate)
        {
            if (_closed)
                return;
            _queue.Enqueue(
                new ScheduledTimer(timer, version),
                new ZLinkTimerScheduleKey(
                    dueAt.UtcTicks,
                    ++_nextSequence));
        }
        SignalWake();
    }

    public ValueTask DisposeAsync()
    {
        lock (_disposeGate)
        {
            if (_disposeTask is not null)
                return new ValueTask(_disposeTask);

            _disposeTask = DisposeCoreAsync();
            return new ValueTask(_disposeTask);
        }
    }

    private async Task DisposeCoreAsync()
    {
        try
        {
            lock (_gate)
                _closed = true;
            _stopSource.Cancel();
            SignalWake();
            await _pump.ConfigureAwait(false);
        }
        finally
        {
            _wake.Dispose();
            _stopSource.Dispose();
            lock (_gate)
            {
                _queue.Clear();
                _timers.Clear();
            }
        }
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (TryTakeDue(out var dueTimer, out var delay))
                {
                    if (dueTimer.Timer.IsScheduleCurrent(dueTimer.Version))
                        dueTimer.Timer.NotifyDue(dueTimer.Version);
                    continue;
                }

                if (delay is null)
                {
                    await _wake.WaitAsync(cancellationToken).ConfigureAwait(false);
                    continue;
                }

                using var waitCancellation =
                    CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                var wake = _wake.WaitAsync(waitCancellation.Token);
                var deadline = Task.Delay(delay.Value, waitCancellation.Token);
                await Task.WhenAny(wake, deadline).ConfigureAwait(false);
                waitCancellation.Cancel();
                try
                {
                    await Task.WhenAll(wake, deadline).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                    when (waitCancellation.IsCancellationRequested)
                {
                    // The other wait won, or the scheduler is stopping.
                }
            }
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
        }
    }

    private bool TryTakeDue(
        out ScheduledTimer dueTimer,
        out TimeSpan? delay)
    {
        lock (_gate)
        {
            if (_queue.Count == 0)
            {
                dueTimer = default;
                delay = null;
                return false;
            }

            _queue.TryPeek(out _, out var key);
            var nowTicks = DateTimeOffset.UtcNow.UtcTicks;
            if (key.UtcTicks > nowTicks)
            {
                dueTimer = default;
                delay = TimeSpan.FromTicks(key.UtcTicks - nowTicks);
                return false;
            }

            dueTimer = _queue.Dequeue();
            delay = TimeSpan.Zero;
            return true;
        }
    }

    private void SignalWake()
    {
        try
        {
            if (_wake.CurrentCount == 0)
                _wake.Release();
        }
        catch (SemaphoreFullException)
        {
        }
        catch (ObjectDisposedException)
        {
        }
    }

    private readonly record struct ScheduledTimer(
        ZLinkTimer Timer,
        long Version);

    private readonly record struct ZLinkTimerScheduleKey(
        long UtcTicks,
        long Sequence) : IComparable<ZLinkTimerScheduleKey>
    {
        public int CompareTo(ZLinkTimerScheduleKey other)
        {
            var byDeadline = UtcTicks.CompareTo(other.UtcTicks);
            return byDeadline != 0
                ? byDeadline
                : Sequence.CompareTo(other.Sequence);
        }
    }
}
