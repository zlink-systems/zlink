namespace Zlink.Framework.Contracts.Timers;

public interface IZLinkTimer : IAsyncDisposable
{
    bool IsDisposed { get; }

    ValueTask CancelAsync();
}

public sealed record ZLinkTimerOptions
{
    public ZLinkTimerOverrunPolicy OverrunPolicy { get; init; } =
        ZLinkTimerOverrunPolicy.SkipLateTicks;

    public int MaxCatchUpTicks { get; init; } = 1;

    public bool StopOnUnhandledException { get; init; }
}

public enum ZLinkTimerOverrunPolicy
{
    SkipLateTicks = 1,
    CatchUpBounded = 2,
    DelayNextTick = 3
}

public readonly record struct ZLinkTimerTick(
    string Name,
    ulong DeliveryIndex,
    ulong ScheduledIndex,
    TimeSpan Period,
    DateTimeOffset ScheduledAt,
    DateTimeOffset StartedAt,
    TimeSpan ScheduledElapsed,
    TimeSpan StartedElapsed,
    TimeSpan Delay,
    ulong SkippedTicks);
