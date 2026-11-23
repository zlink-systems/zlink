namespace Zlink.Framework.Locations.Redis.Tests;

/// <summary>Controls the release wait's monotonic clock and keeps wall-clock time real.</summary>
internal sealed class MonotonicTimeProvider : TimeProvider
{
    private long _timestamp;

    public override long GetTimestamp() => Interlocked.Read(ref _timestamp);

    public override long TimestampFrequency => TimeSpan.TicksPerSecond;

    public void Advance(TimeSpan delta) => Interlocked.Add(ref _timestamp, delta.Ticks);
}
