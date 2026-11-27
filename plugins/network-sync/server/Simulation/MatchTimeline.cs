namespace Zlink.NetworkSync.Simulation;

internal sealed class MatchTimeline(long frequency, int tickRate, long origin)
{
    private long _origin = origin;

    public TickContext LatestTick { get; private set; } = new(1, 0, origin);

    public long TimestampFrequency => frequency;

    public long TickAt(long timestamp) =>
        checked((long)((Int128)unchecked(timestamp - _origin) * tickRate / frequency));

    public TickContext At(long tick) =>
        new(LatestTick.Epoch, tick, unchecked(_origin + (long)TickOffset(tick)));

    public Int128 ClockTicksUntil(long tick, long timestamp) =>
        TickOffset(tick) - unchecked(timestamp - _origin);

    public Int128 BudgetClockTicks(TimeSpan? budget) =>
        budget is { } duration
            ? DivideUp((Int128)duration.Ticks * frequency, TimeSpan.TicksPerSecond)
            : DivideUp(frequency, 2L * tickRate);

    public TimeSpan Duration(Int128 clockTicks)
    {
        var magnitude = DivideUp(Int128.Abs(clockTicks) * TimeSpan.TicksPerSecond, frequency);
        var ticks = clockTicks < 0 ? -magnitude : magnitude;
        return TimeSpan.FromTicks((long)Int128.Clamp(ticks, long.MinValue, long.MaxValue));
    }

    public void Complete(TickContext tick) => LatestTick = tick;

    public void Resync(long timestamp)
    {
        LatestTick = new(checked(LatestTick.Epoch + 1), 0, timestamp);
        _origin = timestamp;
    }

    // Each logical deadline is the first representable timestamp at or after the ideal tick boundary.
    private Int128 TickOffset(long tick) => DivideUp((Int128)tick * frequency, tickRate);

    private static Int128 DivideUp(Int128 value, long divisor) => (value + divisor - 1) / divisor;
}
