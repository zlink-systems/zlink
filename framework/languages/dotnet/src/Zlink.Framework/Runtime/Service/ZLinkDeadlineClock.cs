namespace Zlink.Framework.Runtime.Service;

// Absolute operation deadlines must continue moving forward when the system
// wall clock is adjusted backwards.
internal sealed class ZLinkDeadlineClock
{
    private readonly TimeProvider _timeProvider;
    private readonly object _gate = new();
    private DateTimeOffset _observedUtc;
    private long _observedTimestamp;

    internal ZLinkDeadlineClock(TimeProvider timeProvider)
    {
        _timeProvider = timeProvider ?? throw new ArgumentNullException(nameof(timeProvider));
        _observedUtc = _timeProvider.GetUtcNow();
        _observedTimestamp = _timeProvider.GetTimestamp();
    }

    internal long GetUnixTimeMilliseconds()
    {
        lock (_gate)
        {
            var wallUtc = _timeProvider.GetUtcNow();
            var timestamp = _timeProvider.GetTimestamp();
            var elapsed = _timeProvider.GetElapsedTime(
                _observedTimestamp,
                timestamp);
            if (elapsed < TimeSpan.Zero)
                elapsed = TimeSpan.Zero;

            var monotonicUtc = _observedUtc.Add(elapsed);
            _observedUtc = wallUtc > monotonicUtc ? wallUtc : monotonicUtc;
            _observedTimestamp = timestamp;
            return _observedUtc.ToUnixTimeMilliseconds();
        }
    }
}
