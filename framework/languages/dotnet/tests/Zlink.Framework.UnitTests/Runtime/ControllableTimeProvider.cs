namespace Zlink.Framework.UnitTests;

internal sealed class ControllableTimeProvider : TimeProvider
{
    private readonly object _gate = new();
    private readonly HashSet<ControlledTimer> _timers = [];
    private DateTimeOffset _utcNow =
        new(2026, 7, 2, 0, 0, 0, TimeSpan.Zero);
    private long _timestamp;

    public override DateTimeOffset GetUtcNow()
    {
        lock (_gate) return _utcNow;
    }

    public override long GetTimestamp() => Volatile.Read(ref _timestamp);

    public override long TimestampFrequency => TimeSpan.TicksPerSecond;

    internal int ActiveTimerCount
    {
        get
        {
            lock (_gate) return _timers.Count;
        }
    }

    public override ITimer CreateTimer(
        TimerCallback callback,
        object? state,
        TimeSpan dueTime,
        TimeSpan period)
    {
        var timer = new ControlledTimer(this, callback, state);
        lock (_gate)
        {
            _timers.Add(timer);
            timer.ChangeCore(_timestamp, dueTime, period);
        }
        return timer;
    }

    internal void AdvanceMonotonic(TimeSpan delta)
    {
        if (delta < TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(delta));
        List<(TimerCallback Callback, object? State)> due = [];
        lock (_gate)
        {
            _timestamp = checked(_timestamp + delta.Ticks);
            foreach (var timer in _timers)
                if (timer.TryTakeDue(_timestamp, out var callback))
                    due.Add(callback);
        }
        foreach (var callback in due)
            callback.Callback(callback.State);
    }

    private sealed class ControlledTimer(
        ControllableTimeProvider owner,
        TimerCallback callback,
        object? state) : ITimer
    {
        private long? _dueAt;
        private TimeSpan _period;
        private bool _disposed;

        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            lock (owner._gate)
            {
                if (_disposed) return false;
                ChangeCore(owner._timestamp, dueTime, period);
                return true;
            }
        }

        internal void ChangeCore(
            long now,
            TimeSpan dueTime,
            TimeSpan period)
        {
            _period = period;
            _dueAt = dueTime == Timeout.InfiniteTimeSpan
                ? null
                : checked(now + dueTime.Ticks);
        }

        internal bool TryTakeDue(
            long now,
            out (TimerCallback Callback, object? State) due)
        {
            if (_disposed || _dueAt is not { } dueAt || dueAt > now)
            {
                due = default;
                return false;
            }
            _dueAt = _period > TimeSpan.Zero
                     && _period != Timeout.InfiniteTimeSpan
                ? checked(now + _period.Ticks)
                : null;
            due = (callback, state);
            return true;
        }

        public void Dispose()
        {
            lock (owner._gate)
            {
                if (_disposed) return;
                _disposed = true;
                _dueAt = null;
                owner._timers.Remove(this);
            }
        }

        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }
    }
}
