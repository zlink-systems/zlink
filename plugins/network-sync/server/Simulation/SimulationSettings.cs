namespace Zlink.NetworkSync.Simulation;

internal sealed record SimulationSettings
{
    public int TickRate { get; init; } = 30;
    public int SnapshotRate { get; init; } = 20;
    public int MaxTicksPerWakeup { get; init; } = 4;
    public TimeSpan? WakeupBudget { get; init; }
    public TimeSpan MaxBacklog { get; init; } = TimeSpan.FromMilliseconds(500);
    public OverloadPolicy OverloadPolicy { get; init; } = OverloadPolicy.Resync;
    public int MaxInputBytes { get; init; } = 4096;
    public int MaxInputsPerSecond { get; init; } = 120;
    public int? MaxFutureTicks { get; init; }
    public int MaxHeldTicks { get; init; } = 5;

    internal void Validate(long timestampFrequency)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(TickRate);
        ArgumentOutOfRangeException.ThrowIfGreaterThan((long)TickRate, timestampFrequency, nameof(TickRate));
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(SnapshotRate);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxTicksPerWakeup);
        if (WakeupBudget is { } budget && budget <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(WakeupBudget));
        if (MaxBacklog <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(MaxBacklog));
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxInputBytes);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxInputsPerSecond);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxFutureTicks ?? TickRate, nameof(MaxFutureTicks));
        ArgumentOutOfRangeException.ThrowIfNegative(MaxHeldTicks);
    }
}

internal enum OverloadPolicy
{
    Resync
}
