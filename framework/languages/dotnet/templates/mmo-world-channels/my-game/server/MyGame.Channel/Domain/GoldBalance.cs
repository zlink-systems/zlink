using MyGame.Shared;

namespace MyGame.Channel.Domain;

public sealed record GoldEarnDecision(long? NewValue, string? Reason, long RetryAfterMs)
{
    public bool Accepted => Reason is null;
}

public sealed record GoldSnapshot(long Value, long EarnCooldownRemainingMs);

public sealed class GoldBalance
{
    private static readonly TimeSpan EarnInterval = TimeSpan.FromMilliseconds(GameRules.EarnIntervalMs);
    private readonly TimeProvider _clock;
    private long? _lastEarnTimestamp;

    public GoldBalance(long value, TimeProvider clock)
    {
        _clock = clock ?? throw new ArgumentNullException(nameof(clock));
        Value = ValidateValue(value);
    }

    public long Value { get; private set; }

    public GoldEarnDecision EvaluateEarn()
    {
        var retryAfterMs = EarnCooldownRemainingMs();
        if (retryAfterMs > 0)
            return new(null, Rejection.EarnTooSoon, retryAfterMs);

        if (Value > GameRules.MaxInteger - GameRules.EarnAmount)
            return new(null, Rejection.GoldLimit, 0);

        return new(Value + GameRules.EarnAmount, null, 0);
    }

    public void CommitEarn(GoldEarnDecision decision)
    {
        if (!decision.Accepted || decision.NewValue != Value + GameRules.EarnAmount)
            throw new InvalidOperationException("Only the accepted gold decision can be committed.");

        Value = decision.NewValue.Value;
        _lastEarnTimestamp = _clock.GetTimestamp();
    }

    public GoldSnapshot Capture() => new(Value, EarnCooldownRemainingMs());

    public void Restore(GoldSnapshot snapshot)
    {
        Value = ValidateValue(snapshot.Value);
        if (
            snapshot.EarnCooldownRemainingMs < 0
            || snapshot.EarnCooldownRemainingMs > GameRules.EarnIntervalMs
        )
            throw new ArgumentOutOfRangeException(nameof(snapshot));

        if (snapshot.EarnCooldownRemainingMs == 0)
        {
            _lastEarnTimestamp = null;
            return;
        }

        var elapsed = EarnInterval - TimeSpan.FromMilliseconds(snapshot.EarnCooldownRemainingMs);
        var timestampOffset = ToTimestampUnits(elapsed, _clock.TimestampFrequency);
        _lastEarnTimestamp = _clock.GetTimestamp() - timestampOffset;
    }

    private long EarnCooldownRemainingMs()
    {
        if (_lastEarnTimestamp is not long lastEarnTimestamp)
            return 0;

        var remaining = EarnInterval - _clock.GetElapsedTime(lastEarnTimestamp);
        return remaining <= TimeSpan.Zero ? 0 : CeilingMilliseconds(remaining);
    }

    private static long ValidateValue(long value) =>
        value is >= 0 and <= GameRules.MaxInteger
            ? value
            : throw new ArgumentOutOfRangeException(nameof(value));

    private static long CeilingMilliseconds(TimeSpan duration) =>
        (duration.Ticks + TimeSpan.TicksPerMillisecond - 1) / TimeSpan.TicksPerMillisecond;

    private static long ToTimestampUnits(TimeSpan duration, long frequency) =>
        (long)((decimal)duration.Ticks * frequency / TimeSpan.TicksPerSecond);
}
